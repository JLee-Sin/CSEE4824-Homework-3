#include "bp_sim.h"

namespace {

struct Entry {
	std::string name;
	BranchSimulatorFactory make;
};

std::vector<Entry>& Registry() {
	static std::vector<Entry> r;
	return r;
}

}

bool RegisterBranchSimulator(const char* name, BranchSimulatorFactory factory) {
	Registry().push_back(Entry{name, factory});
	return true;
}

std::string SimDriver::Registered() {
	std::string s;
	for(const Entry& e : Registry()) {
		s += (s.empty() ? "" : " ") + e.name;
	}
	return s.empty() ? "(none)" : s;
}

SimDriver::~SimDriver() {
	for(BranchSimulator* s : sims_) {
		delete s;
	}
}

bool SimDriver::Init(const std::string& specs, std::string* err) {
	size_t p = 0;
	while(p < specs.size()) {
		size_t q = specs.find(',', p);
		if(q == std::string::npos) {
			q = specs.size();
		}
		const std::string spec = specs.substr(p, q - p);
		p = q + 1;
		if(spec.empty()) {
			continue;
		}
		const size_t c = spec.find(':');
		const std::string name = spec.substr(0, c);
		const std::string args = c == std::string::npos ? "" : spec.substr(c + 1);
		const Entry* e = nullptr;
		for(const Entry& r : Registry()) {
			if(r.name == name) {
				e = &r;
				break;
			}
		}
		if(!e) {
			*err = "unknown simulator '" + name + "' (registered: " + Registered() + ")";
			return false;
		}
		err->clear();
		BranchSimulator* s = e->make(args, err);
		if(!s) {
			if(err->empty()) {
				*err = "cannot create simulator '" + spec + "'";
			}
			return false;
		}
		std::string key = spec;
		for(char& ch : key) {
			if(!isalnum((unsigned char)ch) && ch != '_' && ch != '-') {
				ch = '_';
			}
		}
		if(key == "tool" || key == "check") {
			delete s;
			*err = "simulator spec '" + spec + "' would use tool's '" + key + ".' keys";
			return false;
		}
		for(const std::string& k : keys_) {
			if(k == key) {
				delete s;
				*err = "simulator spec '" + spec + "' would report under same name as other spec";
				return false;
			}
		}
		sims_.push_back(s);
		specs_.push_back(spec);
		keys_.push_back(key);
	}
	miss_.assign(sims_.size() * BR_NTYPES, 0);
	return true;
}

bool SimDriver::OnEvent(const BranchEvent& ev) {
	n_[ev.type]++;
	bool primaryMiss = false;
	for(size_t i = 0; i < sims_.size(); i++) {
		if(sims_[i]->OnEvent(ev)) {
			miss_[i * BR_NTYPES + ev.type]++;
			if(i == 0) {
				primaryMiss = true;
			}
		}
	}
	return primaryMiss;
}

void SimDriver::Report(FILE* f, uint64_t insts) const {
	auto mpki = [&](uint64_t m) {
		return insts ? 1000.0 * m / insts : 0.0;
	};
	for(size_t i = 0; i < sims_.size(); i++) {
		const std::string p = keys_[i] + ".";
		const uint64_t* m = &miss_[i * BR_NTYPES];
		uint64_t total = 0;
		fprintf(f, "%sspec = %s\n", p.c_str(), specs_[i].c_str());
		for(int t = 0; t < BR_NTYPES; t++) {
			fprintf(f, "%s%s.mispred = %llu\n", p.c_str(), BrTypeName((BrType)t), (unsigned long long)m[t]);
			total += m[t];
		}
		const uint64_t nc = n_[BR_COND];
		fprintf(f, "%scond_accuracy = %.6f\n", p.c_str(), nc ? 1.0 - (double)m[BR_COND] / nc : 0.0);
		fprintf(f, "%scond_mpki = %.4f\n", p.c_str(), mpki(m[BR_COND]));
		fprintf(f, "%stotal_mispred = %llu\n", p.c_str(), (unsigned long long)total);
		fprintf(f, "%stotal_mpki = %.4f\n", p.c_str(), mpki(total));
		sims_[i]->Report(f, p, insts);
	}
}
