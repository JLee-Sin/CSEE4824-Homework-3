#include <cstring>
#include "../bp_sim.h"

namespace {

const int kLhtBits = 10;
const int kLocalHist = 10;
const int kGhrBits = 12;
const int kBtbBits = 8;
const int kTagBits = 16;
const int kTargetBits = 32;
const int kTypeBits = 3;
const unsigned kRasDepth = 16;
const int kRasCountBits = 5;

inline void Sat(uint8_t& c, uint8_t max, bool up) {
	if(up) {
		if(c < max) {
			c++;
		}
	} else if(c > 0) {
		c--;
	}
}

inline uint64_t Expand(uint64_t pc, uint32_t low32) {
	return (pc & ~0xffffffffULL) | low32;
}

class Baseline : public BranchSimulator {
public:
	Baseline()
			: lht_(1u << kLhtBits, 0), lpt_(1u << kLocalHist, 4), gpt_(1u << kGhrBits, 2),
			choice_(1u << kGhrBits, 2), btb_(1u << kBtbBits) {}

	bool OnEvent(const BranchEvent& ev) override {
		const uint64_t pc = ev.pc;
		const uint64_t seq = pc + ev.size;

		uint16_t& lhist = lht_[pc & ((1u << kLhtBits) - 1)];
		const bool localPred = lpt_[lhist] >= 4;
		const bool globalPred = gpt_[ghr_] >= 2;
		const bool useGlobal = choice_[ghr_] >= 2;
		const bool dirPred = useGlobal ? globalPred : localPred;

		Btb& e = btb_[pc & ((1u << kBtbBits) - 1)];
		const bool hit = e.valid && e.tag == ((pc >> kBtbBits) & ((1u << kTagBits) - 1));
		Source src;
		uint64_t pred;
		if(!hit) {
			src = NONE;
			pred = seq;
		} else if(e.type == BR_COND) {
			src = dirPred ? COND_T : COND_NT;
			pred = dirPred ? Expand(pc, e.target) : seq;
		} else if(e.type == BR_RET) {
			if(rasCount_) {
				src = RAS;
				pred = Expand(pc, ras_[rasCount_ - 1]);
			} else {
				src = RAS_EMPTY;
				pred = Expand(pc, e.target);
			}
		} else {
			src = TARGET;
			pred = Expand(pc, e.target);
		}

		const bool miss = pred != ev.n_pc;
		if(miss) {
			switch(src) {
			case NONE:
				missBtb_++;
				break;
			case COND_NT:
				missDir_++;
				break;
			case COND_T:
				if(!ev.taken) {
					missDir_++;
				} else {
					missTarget_++;
				}
				break;
			case RAS:
				missRas_++;
				break;
			case RAS_EMPTY:
				missRasEmpty_++;
				break;
			case TARGET:
				missTarget_++;
				break;
			}
			if(src != NONE && src != COND_NT && (uint32_t)pred == (uint32_t)ev.n_pc) {
				missHigh_++;
			}
		}

		if(ev.type == BR_COND) {
			dirMiss_ += dirPred != ev.taken;
			localMiss_ += localPred != ev.taken;
			globalMiss_ += globalPred != ev.taken;
			choseGlobal_ += useGlobal;
			if(localPred != globalPred) {
				Sat(choice_[ghr_], 3, globalPred == ev.taken);
			}
			Sat(lpt_[lhist], 7, ev.taken);
			Sat(gpt_[ghr_], 3, ev.taken);
			lhist = ((lhist << 1) | ev.taken) & ((1u << kLocalHist) - 1);
			ghr_ = ((ghr_ << 1) | ev.taken) & ((1u << kGhrBits) - 1);
		}
		if(ev.taken) {
			e.valid = true;
			e.tag = (pc >> kBtbBits) & ((1u << kTagBits) - 1);
			e.target = (uint32_t)ev.n_pc;
			e.type = ev.type;
		}
		if(BrIsCall(ev.type)) {
			Push((uint32_t)seq);
		}
		if(ev.type == BR_RET) {
			Pop();
		}
		return miss;
	}

	void Report(FILE* f, const std::string& p, uint64_t insts) const override {
		const uint64_t dirBits = (1ULL << kLhtBits) * kLocalHist + (1ULL << kLocalHist) * 3 +
				(1ULL << kGhrBits) * 2 * 2 + kGhrBits;
		const uint64_t btbBits = (1ULL << kBtbBits) * (1 + kTagBits + kTargetBits + kTypeBits);
		const uint64_t rasBits = kRasDepth * kTargetBits + kRasCountBits;
		auto mpki = [&](uint64_t m) {
			return insts ? 1000.0 * m / insts : 0.0;
		};
		auto line = [&](const char* k, uint64_t v) {
			fprintf(f, "%s%s = %llu\n", p.c_str(), k, (unsigned long long)v);
		};
		line("bits.direction", dirBits);
		line("bits.btb", btbBits);
		line("bits.ras", rasBits);
		line("bits.total", dirBits + btbBits + rasBits);
		line("dir.cond_mispred", dirMiss_);
		fprintf(f, "%sdir.cond_mpki = %.4f\n", p.c_str(), mpki(dirMiss_));
		line("dir.local_mispred", localMiss_);
		line("dir.global_mispred", globalMiss_);
		line("dir.chose_global", choseGlobal_);
		line("miss.btb_miss", missBtb_);
		line("miss.direction", missDir_);
		line("miss.btb_target", missTarget_);
		line("miss.ras", missRas_);
		line("miss.ras_underflow", missRasEmpty_);
		line("miss.high_bits", missHigh_);
		line("ras.overflows", rasOverflow_);
		line("ras.underflows", rasUnderflow_);
	}

private:
	enum Source {
		NONE,
		COND_NT,
		COND_T,
		RAS,
		RAS_EMPTY,
		TARGET
	};

	struct Btb {
		bool valid = false;
		uint16_t tag = 0;
		uint32_t target = 0;
		uint8_t type = 0;
	};

	void Push(uint32_t a) {
		if(rasCount_ == kRasDepth) {
			memmove(ras_, ras_ + 1, (kRasDepth - 1) * sizeof ras_[0]);
			rasCount_--;
			rasOverflow_++;
		}
		ras_[rasCount_++] = a;
	}

	void Pop() {
		if(rasCount_ == 0) {
			rasUnderflow_++;
			return;
		}
		rasCount_--;
	}

	std::vector<uint16_t> lht_;
	std::vector<uint8_t> lpt_;
	std::vector<uint8_t> gpt_;
	std::vector<uint8_t> choice_;
	uint32_t ghr_ = 0;
	std::vector<Btb> btb_;
	uint32_t ras_[kRasDepth] = {};
	unsigned rasCount_ = 0;

	uint64_t dirMiss_ = 0;
	uint64_t localMiss_ = 0;
	uint64_t globalMiss_ = 0;
	uint64_t choseGlobal_ = 0;
	uint64_t missBtb_ = 0;
	uint64_t missDir_ = 0;
	uint64_t missTarget_ = 0;
	uint64_t missRas_ = 0;
	uint64_t missRasEmpty_ = 0;
	uint64_t missHigh_ = 0;
	uint64_t rasOverflow_ = 0;
	uint64_t rasUnderflow_ = 0;
};

BranchSimulator* MakeBaseline(const std::string& args, std::string* err) {
	if(!args.empty()) {
		*err = "baseline takes no arguments (sizes are fixed by the spec)";
		return nullptr;
	}
	return new Baseline;
}

}

REGISTER_BRANCH_SIMULATOR("baseline", MakeBaseline);
