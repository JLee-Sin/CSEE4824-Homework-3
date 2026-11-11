#ifndef BRANCH_EVENT_H
#define BRANCH_EVENT_H

#include <cstdio>
#include <cstdint>

enum BrType : uint8_t {
	BR_COND = 0,
	BR_JMP_DIR,
	BR_JMP_IND,
	BR_CALL_DIR,
	BR_CALL_IND,
	BR_RET,
	BR_NTYPES
};

static inline const char* BrTypeName(BrType t) {
	static const char* const n[BR_NTYPES] = {"COND", "JMP_DIR", "JMP_IND", "CALL_DIR", "CALL_IND", "RET"};
	return t < BR_NTYPES ? n[t] : "?";
}

static inline bool BrIsDirect(BrType t) {
	return t == BR_COND || t == BR_JMP_DIR || t == BR_CALL_DIR;
}

static inline bool BrIsCall(BrType t) {
	return t == BR_CALL_DIR || t == BR_CALL_IND;
}

static inline bool BrHasFallThrough(BrType t) {
	return t == BR_COND || BrIsCall(t);
}

struct BranchEvent {
	uint64_t seq;
	uint64_t pc;
	uint64_t size;
	BrType type;
	bool taken;
	uint64_t n_pc;
	uint64_t fallthrough;
	uint64_t target;
	uint64_t icount;
};

static inline void WriteCSVHeader(FILE* f, bool disasm) {
	fprintf(f, "seq,pc,size,type,direct,taken,n_pc,fallthrough,target,icount%s\n", disasm ? ",disasm" : "");
}

static inline void WriteCSVRow(FILE* f, const BranchEvent& e, const char* disasm) {
	fprintf(f, "%llu,0x%llx,%llu,%s,%d,%d,0x%llx,",
			(unsigned long long)e.seq, (unsigned long long)e.pc, (unsigned long long)e.size, BrTypeName(e.type),
			BrIsDirect(e.type) ? 1:0, e.taken ? 1:0, (unsigned long long)e.n_pc);
	if(BrHasFallThrough(e.type)) {
		fprintf(f, "0x%llx", (unsigned long long)e.fallthrough);
	}
	fputc(',', f);
	if(BrIsDirect(e.type)) {
		fprintf(f, "0x%llx", (unsigned long long)e.target);
	}
	fprintf(f, ",%llu", (unsigned long long)e.icount);
	if(disasm) {
		fprintf(f, ",\"%s\"", disasm);
	}
	fputc('\n', f);
}

#endif
