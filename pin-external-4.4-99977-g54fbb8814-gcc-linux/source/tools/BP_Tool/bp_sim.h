#ifndef BP_SIM_H
#define BP_SIM_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "branch_event.h"

class BranchSimulator {
public:
	virtual ~BranchSimulator() {}
	virtual bool OnEvent(const BranchEvent& ev) = 0;
	virtual void Report(FILE* f, const std::string& prefix, uint64_t instructions) const {}
};

typedef BranchSimulator* (*BranchSimulatorFactory)(const std::string& args, std::string* err);

bool RegisterBranchSimulator(const char* name, BranchSimulatorFactory factory);

#define BP_SIM_CONCAT2(a, b) a##b
#define BP_SIM_CONCAT(a, b) BP_SIM_CONCAT2(a, b)
#define REGISTER_BRANCH_SIMULATOR(name, factory) \
	static const bool BP_SIM_CONCAT(bp_sim_registered_, __LINE__) \
			__attribute__((unused)) = RegisterBranchSimulator((name), (factory))

class SimDriver {
public:
	~SimDriver();
	bool Init(const std::string& specs, std::string* err);
	size_t Count() const {
		return sims_.size();
	}
	bool OnEvent(const BranchEvent& ev);
	void Report(FILE* f, uint64_t instructions) const;
	static std::string Registered();

private:
	std::vector<BranchSimulator*> sims_;
	std::vector<std::string> specs_;
	std::vector<std::string> keys_;
	std::vector<uint64_t> miss_;
	uint64_t n_[BR_NTYPES] = {};
};

#endif
