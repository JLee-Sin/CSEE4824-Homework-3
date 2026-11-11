#include "pin.H"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "bp_sim.h"
#include "branch_event.h"

static KNOB<std::string> KnobOut(KNOB_MODE_WRITEONCE, "pintool", "o", "bp.out",
		"statistics output file (key = value lines)");
static KNOB<std::string> KnobSim(KNOB_MODE_WRITEONCE, "pintool", "sim", "",
		"comma-separated simulators to run side by side, each name[:args] as registered with "
		"REGISTER_BRANCH_SIMULATOR; the first is the primary one (empty = trace/count only)");
static KNOB<UINT64> KnobMaxInsts(KNOB_MODE_WRITEONCE, "pintool", "max_insts", "0",
		"simulate only the first N dynamic instructions (0 = no limit). The window ends just "
		"before the first control-flow instruction whose cumulative count exceeds N");
static KNOB<UINT64> KnobMaxEvents(KNOB_MODE_WRITEONCE, "pintool", "max_events", "0",
		"simulate only the first N control-flow events (0 = no limit)");
static KNOB<std::string> KnobLimitMode(KNOB_MODE_WRITEONCE, "pintool", "limit_mode", "exit",
		"what to do when a limit is reached: exit (PIN_ExitApplication, fastest) or detach "
		"(PIN_Detach: the application runs on natively to completion)");
static KNOB<std::string> KnobTrace(KNOB_MODE_WRITEONCE, "pintool", "trace", "",
		"CSV event trace file for validation (empty = none)");
static KNOB<UINT64> KnobTraceStart(KNOB_MODE_WRITEONCE, "pintool", "trace_start", "0",
		"first event (seq) written to the trace");
static KNOB<UINT64> KnobTraceLen(KNOB_MODE_WRITEONCE, "pintool", "trace_len", "1000",
		"number of events written to the trace (0 = no limit; beware of file size)");
static KNOB<BOOL> KnobTraceDisasm(KNOB_MODE_WRITEONCE, "pintool", "trace_disasm", "1",
		"append each branch's disassembly to the trace");
static KNOB<UINT32> KnobTop(KNOB_MODE_WRITEONCE, "pintool", "top", "20",
		"report the N static branches with the most mispredictions (primary simulator)");
static KNOB<BOOL> KnobCheck(KNOB_MODE_WRITEONCE, "pintool", "check", "0",
		"self-check (slow): verify each event's n_pc against the next executed instruction "
		"and cross-check the instruction count with an independent per-instruction count");

struct StaticBranch {
	ADDRINT pc;
	ADDRINT fallthrough;
	ADDRINT direct_target;
	UINT32 size;
	BrType type;
	UINT64 exec;
	UINT64 mispred;
	std::string disasm;
};

static UINT64 g_icount = 0;
static ADDRINT g_repPc = 0;
static UINT64 g_seq = 0;
static UINT64 g_maxInsts = ~0ULL;
static UINT64 g_maxEvents = ~0ULL;
static bool g_detachOnLimit = false;
static bool g_stopped = false;
static UINT64 g_windowIcount = 0;
static const char* g_stopReason = "application_exit";
static bool g_reported = false;
static UINT64 g_traceStart = 0;
static UINT64 g_traceEnd = 0;
static FILE* g_trace = nullptr;
static bool g_traceDisasm = true;
static UINT32 g_threads = 0;
static std::unordered_map<ADDRINT, StaticBranch*> g_static;
static std::vector<StaticBranch*> g_allStatic;
static SimDriver g_sim;
static UINT64 g_typeN[BR_NTYPES];
static UINT64 g_typeTaken[BR_NTYPES];

static UINT64 g_ignoredStatic = 0;
static UINT64 g_ignoredDyn = 0;
static UINT64 g_condIndirect = 0;
static UINT64 g_condDisagree = 0;
static UINT64 g_dirDisagree = 0;
static UINT64 g_directTargetBad = 0;
static UINT64 g_noncondNotTaken = 0;

static bool g_check = false;
static UINT64 g_chkIcount = 0;
static ADDRINT g_chkRepPc = 0;
static bool g_chkPending = false;
static ADDRINT g_chkExpect = 0;
static ADDRINT g_chkFrom = 0;
static UINT64 g_chkNextOk = 0;
static UINT64 g_chkNextBad = 0;
static UINT64 g_chkIcountBad = 0;

static VOID PIN_FAST_ANALYSIS_CALL CountBbl(UINT32 n) {
	g_icount += n;
	g_repPc = 0;
}

static inline bool RepStarts(ADDRINT pc, BOOL first, BOOL executing, ADDRINT* cur) {
	if(first) {
		*cur = pc;
		return true;
	}
	if(!executing) {
		bool fresh = *cur != pc;
		*cur = 0;
		return fresh;
	}
	return false;
}

static VOID PIN_FAST_ANALYSIS_CALL CountRep(ADDRINT pc, BOOL first, BOOL executing) {
	if(RepStarts(pc, first, executing, &g_repPc)) {
		g_icount++;
	}
}

static VOID PIN_FAST_ANALYSIS_CALL CountIgnored() {
	g_ignoredDyn++;
}

static VOID CheckNext(ADDRINT pc) {
	if(!g_chkPending) {
		return;
	}
	g_chkPending = false;
	if(pc == g_chkExpect) {
		g_chkNextOk++;
	} else if(g_chkNextBad++ < 10) {
		fprintf(stderr, "bp_tool check: event at %#lx predicted next %#lx but %#lx executed\n",
				(unsigned long)g_chkFrom, (unsigned long)g_chkExpect, (unsigned long)pc);
	}
}

static VOID PIN_FAST_ANALYSIS_CALL CheckIns(ADDRINT pc) {
	g_chkIcount++;
	g_chkRepPc = 0;
	CheckNext(pc);
}

static VOID PIN_FAST_ANALYSIS_CALL CheckRep(ADDRINT pc, BOOL first, BOOL executing) {
	if(RepStarts(pc, first, executing, &g_chkRepPc)) {
		g_chkIcount++;
		CheckNext(pc);
	}
}

static VOID StopAtLimit(UINT64 windowIcount, const char* why) {
	g_stopped = true;
	g_windowIcount = windowIcount;
	g_stopReason = why;
	if(g_detachOnLimit) {
		PIN_Detach();
	} else {
		PIN_ExitApplication(0);
	}
}

static VOID PIN_FAST_ANALYSIS_CALL OnBranch(StaticBranch* sb, BOOL taken, ADDRINT target) {
	if(g_stopped) {
		return;
	}
	if(g_icount > g_maxInsts) {
		StopAtLimit(g_icount - 1, "max_insts");
		return;
	}
	if(g_seq >= g_maxEvents) {
		StopAtLimit(g_icount - 1, "max_events");
		return;
	}

	BranchEvent ev;
	ev.seq = g_seq++;
	ev.pc = sb->pc;
	ev.size = sb->size;
	ev.type = sb->type;
	ev.taken = taken != 0;
	ev.icount = g_icount;
	ev.fallthrough = BrHasFallThrough(sb->type) ? sb->fallthrough : 0;
	if(BrIsDirect(sb->type)) {
		ev.target = sb->direct_target;
		if(target != sb->direct_target) {
			g_directTargetBad++;
		}
	} else {
		ev.target = target;
	}
	if(sb->type != BR_COND && !ev.taken) {
		g_noncondNotTaken++;
	}
	ev.n_pc = (sb->type == BR_COND && !ev.taken) ? sb->fallthrough : ev.target;

	if(g_check) {
		if(g_chkIcount != g_icount && g_chkIcountBad++ < 10) {
			fprintf(stderr, "bp_tool check: icount mismatch at seq %llu: bbl=%llu ins=%llu\n",
					(unsigned long long)ev.seq, (unsigned long long)g_icount,
					(unsigned long long)g_chkIcount);
		}
		g_chkPending = true;
		g_chkExpect = ev.n_pc;
		g_chkFrom = ev.pc;
	}

	sb->exec++;
	g_typeN[ev.type]++;
	g_typeTaken[ev.type] += ev.taken;
	if(g_sim.Count() && g_sim.OnEvent(ev)) {
		sb->mispred++;
	}

	if(g_trace && ev.seq >= g_traceStart && ev.seq < g_traceEnd) {
		WriteCSVRow(g_trace, ev, g_traceDisasm ? sb->disasm.c_str() : nullptr);
	}
}

static bool Classify(INS ins, BrType* t) {
	if(!INS_IsControlFlow(ins)) {
		return false;
	}
	if(INS_IsXbegin(ins) || INS_IsXend(ins) || INS_IsFarCall(ins) || INS_IsFarJump(ins) ||
			INS_IsFarRet(ins) || INS_Mnemonic(ins).compare(0, 4, "IRET") == 0) {
		g_ignoredStatic++;
		return false;
	}
	const bool direct = INS_IsDirectControlFlow(ins);
	if(direct == (bool)INS_IsIndirectControlFlow(ins)) {
		g_dirDisagree++;
	}
	if(INS_IsRet(ins)) {
		*t = BR_RET;
		return true;
	}
	if(INS_IsCall(ins)) {
		*t = direct ? BR_CALL_DIR : BR_CALL_IND;
		return true;
	}
	if(INS_IsBranch(ins)) {
		const bool cond = INS_Category(ins) == XED_CATEGORY_COND_BR;
		if(cond != (bool)INS_HasFallThrough(ins)) {
			g_condDisagree++;
		}
		if(cond) {
			if(!direct) {
				g_condIndirect++;
				return false;
			}
			*t = BR_COND;
		} else {
			*t = direct ? BR_JMP_DIR : BR_JMP_IND;
		}
		return true;
	}
	g_ignoredStatic++;
	return false;
}

static StaticBranch* GetStatic(INS ins, BrType t) {
	const ADDRINT pc = INS_Address(ins);
	const UINT32 size = (UINT32)INS_Size(ins);
	const ADDRINT dt = BrIsDirect(t) ? INS_DirectControlFlowTargetAddress(ins) : 0;
	auto it = g_static.find(pc);
	if(it != g_static.end() && it->second->size == size && it->second->type == t &&
			it->second->direct_target == dt) {
		return it->second;
	}
	StaticBranch* sb = new StaticBranch;
	sb->pc = pc;
	sb->size = size;
	sb->fallthrough = INS_NextAddress(ins);
	sb->direct_target = dt;
	sb->type = t;
	sb->exec = 0;
	sb->mispred = 0;
	sb->disasm = INS_Disassemble(ins);
	g_static[pc] = sb;
	g_allStatic.push_back(sb);
	return sb;
}

static bool IsRep(INS ins) {
	return INS_HasRealRep(ins) && !INS_IsControlFlow(ins);
}

static VOID InstrumentTrace(TRACE trace, VOID*) {
	for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
		UINT32 plain = 0;
		for(INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
			if(!IsRep(ins)) {
				plain++;
			}
		}
		if(plain) {
			BBL_InsertCall(bbl, IPOINT_BEFORE, (AFUNPTR)CountBbl, IARG_FAST_ANALYSIS_CALL,
					IARG_CALL_ORDER, CALL_ORDER_FIRST, IARG_UINT32, plain, IARG_END);
		}

		for(INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
			if(IsRep(ins)) {
				INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)CountRep, IARG_FAST_ANALYSIS_CALL,
						IARG_CALL_ORDER, CALL_ORDER_FIRST, IARG_INST_PTR,
						IARG_FIRST_REP_ITERATION, IARG_EXECUTING, IARG_END);
			}
			if(g_check) {
				if(IsRep(ins)) {
					INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)CheckRep, IARG_FAST_ANALYSIS_CALL,
							IARG_CALL_ORDER, CALL_ORDER_FIRST + 1, IARG_INST_PTR,
							IARG_FIRST_REP_ITERATION, IARG_EXECUTING, IARG_END);
				} else {
					INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)CheckIns, IARG_FAST_ANALYSIS_CALL,
							IARG_CALL_ORDER, CALL_ORDER_FIRST + 1, IARG_INST_PTR, IARG_END);
				}
			}
			BrType t;
			if(!Classify(ins, &t)) {
				if(INS_IsControlFlow(ins)) {
					INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)CountIgnored, IARG_FAST_ANALYSIS_CALL, IARG_END);
				}
				continue;
			}
			StaticBranch* sb = GetStatic(ins, t);
			if(t == BR_COND) {
				INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)OnBranch, IARG_FAST_ANALYSIS_CALL,
						IARG_CALL_ORDER, CALL_ORDER_DEFAULT, IARG_PTR, sb,
						IARG_BRANCH_TAKEN, IARG_BRANCH_TARGET_ADDR, IARG_END);
			} else {
				INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)OnBranch, IARG_FAST_ANALYSIS_CALL,
						IARG_CALL_ORDER, CALL_ORDER_DEFAULT, IARG_PTR, sb,
						IARG_BOOL, TRUE, IARG_BRANCH_TARGET_ADDR, IARG_END);
			}
		}
	}
}

static VOID ThreadStart(THREADID tid, CONTEXT*, INT32, VOID*) {
	if(++g_threads > 1) {
		fprintf(stderr, "bp_tool: WARNING: thread %u started; this tool supports "
				"single-threaded applications only (results are invalid)\n", tid);
	}
}

static VOID Report(INT32 code) {
	if(g_reported) {
		return;
	}
	g_reported = true;
	if(g_trace) {
		fclose(g_trace);
		g_trace = nullptr;
	}
	FILE* f = fopen(KnobOut.Value().c_str(), "w");
	if(!f) {
		fprintf(stderr, "bp_tool: cannot open %s, writing to stderr\n", KnobOut.Value().c_str());
		f = stderr;
	}
	const UINT64 insts = g_stopped ? g_windowIcount : g_icount;

	fprintf(f, "# bp_tool statistics (key = value). MPKI = 1000 * mispredictions / instructions\n");
	fprintf(f, "tool.exit_code = %d\n", code);
	fprintf(f, "tool.stop_reason = %s\n", g_stopReason);
	fprintf(f, "tool.max_insts = %llu\n", (unsigned long long)(g_maxInsts == ~0ULL ? 0 : g_maxInsts));
	fprintf(f, "tool.max_events = %llu\n", (unsigned long long)(g_maxEvents == ~0ULL ? 0 : g_maxEvents));
	fprintf(f, "tool.threads = %u\n", g_threads);
	fprintf(f, "tool.instructions = %llu\n", (unsigned long long)insts);
	fprintf(f, "tool.cf_events = %llu\n", (unsigned long long)g_seq);
	fprintf(f, "tool.static_branches = %llu\n", (unsigned long long)g_allStatic.size());
	fprintf(f, "tool.ignored_cf_static = %llu\n", (unsigned long long)g_ignoredStatic);
	fprintf(f, "tool.ignored_cf_dynamic = %llu\n", (unsigned long long)g_ignoredDyn);
	fprintf(f, "tool.cond_indirect_static = %llu\n", (unsigned long long)g_condIndirect);
	fprintf(f, "tool.cond_classification_disagree = %llu\n", (unsigned long long)g_condDisagree);
	fprintf(f, "tool.direct_classification_disagree = %llu\n", (unsigned long long)g_dirDisagree);
	fprintf(f, "tool.direct_target_mismatch = %llu\n", (unsigned long long)g_directTargetBad);
	fprintf(f, "tool.noncond_not_taken = %llu\n", (unsigned long long)g_noncondNotTaken);
	if(g_check) {
		fprintf(f, "check.instructions_per_ins = %llu\n", (unsigned long long)g_chkIcount);
		fprintf(f, "check.next_pc_ok = %llu\n", (unsigned long long)g_chkNextOk);
		fprintf(f, "check.next_pc_bad = %llu\n", (unsigned long long)g_chkNextBad);
		fprintf(f, "check.icount_mismatch = %llu\n", (unsigned long long)g_chkIcountBad);
	}
	for(int t = 0; t < BR_NTYPES; t++) {
		fprintf(f, "tool.%s.count = %llu\ntool.%s.taken = %llu\n", BrTypeName((BrType)t),
				(unsigned long long)g_typeN[t], BrTypeName((BrType)t), (unsigned long long)g_typeTaken[t]);
	}
	g_sim.Report(f, insts);

	UINT32 top = g_sim.Count() ? KnobTop.Value() : 0;
	if(top) {
		std::vector<StaticBranch*> v;
		for(StaticBranch* sb : g_allStatic) {
			if(sb->mispred) {
				v.push_back(sb);
			}
		}
		std::sort(v.begin(), v.end(), [](const StaticBranch* a, const StaticBranch* b) {
			return a->mispred != b->mispred ? a->mispred > b->mispred : a->pc < b->pc;
		});
		fprintf(f, "# top mispredicted static branches (primary simulator)\n");
		fprintf(f, "# top,rank,pc,type,exec,mispred,mispred_rate,disasm\n");
		for(UINT32 i = 0; i < top && i < v.size(); i++) {
			fprintf(f, "top,%u,0x%llx,%s,%llu,%llu,%.4f,\"%s\"\n", i + 1, (unsigned long long)v[i]->pc,
					BrTypeName(v[i]->type), (unsigned long long)v[i]->exec,
					(unsigned long long)v[i]->mispred, (double)v[i]->mispred / v[i]->exec,
					v[i]->disasm.c_str());
		}
	}
	if(f != stderr) {
		fclose(f);
	}
}

static VOID Fini(INT32 code, VOID*) {
	Report(code);
}

static VOID Detached(VOID*) {
	Report(0);
}

static INT32 Usage() {
	fprintf(stderr, "bp_tool: control-flow tracer with branch simulator hooks (-sim)\n%s\n",
			KNOB_BASE::StringKnobSummary().c_str());
	return -1;
}

int main(int argc, char* argv[]) {
	if(PIN_Init(argc, argv)) {
		return Usage();
	}

	if(KnobMaxInsts.Value()) {
		g_maxInsts = KnobMaxInsts.Value();
	}
	if(KnobMaxEvents.Value()) {
		g_maxEvents = KnobMaxEvents.Value();
	}
	if(KnobLimitMode.Value() == "detach") {
		g_detachOnLimit = true;
	} else if(KnobLimitMode.Value() != "exit") {
		fprintf(stderr, "bp_tool: -limit_mode must be exit or detach\n");
		return 1;
	}
	g_check = KnobCheck.Value();

	std::string err;
	if(!g_sim.Init(KnobSim.Value(), &err)) {
		fprintf(stderr, "bp_tool: %s\n", err.c_str());
		return 1;
	}

	g_traceDisasm = KnobTraceDisasm.Value();
	if(!KnobTrace.Value().empty()) {
		g_trace = fopen(KnobTrace.Value().c_str(), "w");
		if(!g_trace) {
			fprintf(stderr, "bp_tool: cannot open %s\n", KnobTrace.Value().c_str());
			return 1;
		}
		g_traceStart = KnobTraceStart.Value();
		g_traceEnd = KnobTraceLen.Value() ? g_traceStart + KnobTraceLen.Value() : ~0ULL;
		WriteCSVHeader(g_trace, g_traceDisasm);
	}

	TRACE_AddInstrumentFunction(InstrumentTrace, nullptr);
	PIN_AddThreadStartFunction(ThreadStart, nullptr);
	PIN_AddFiniFunction(Fini, nullptr);
	PIN_AddDetachFunction(Detached, nullptr);
	PIN_StartProgram();
	return 0;
}
