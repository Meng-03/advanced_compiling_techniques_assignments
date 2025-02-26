#include <llvm/IR/Function.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/IntrinsicInst.h>

#include <set>
#include <map>

#include "Dataflow.h"
using namespace llvm;

 #define _DEBUG

struct PointsToInfo {
    std::map<Value *, std::set<Value *>> pointsToSets;
    std::map<Value *, std::set<Value *>> aliases;

    bool operator== (const PointsToInfo &info) const {
        return pointsToSets == info.pointsToSets && aliases == info.aliases;
    }
};

inline raw_ostream &operator<< (raw_ostream &out, const PointsToInfo &info) {
    out << "\nPoints-to Sets: \n";
    for (auto pts: info.pointsToSets) {
        if (pts.first->hasName()) {
            out << pts.first->getName() << " : ";
        } else {
            pts.first->print(out);
            out << " : ";
        }
        for (auto p: pts.second) {
            if (p == NULL) {
                continue;
            }
            if (p->hasName()) {
                out << p->getName() << ", ";
            } else {
                p->print(out);
                out << ", ";
            }
        }
        out << "\n";
    }

    out << "\nAliases: \n";
    for (auto pts: info.aliases) {
        if (pts.first->hasName()) {
            out << pts.first->getName() << " : ";
        } else {
            pts.first->print(out);
            out << " : ";
        }
        for (auto p: pts.second) {
            if (p == NULL) {
                continue;
            }
            if (p->hasName()) {
                out << p->getName() << ", ";
            } else {
                p->print(out);
                out << ", ";
            }
        }
        out << "\n";
    }
    return out;
}

class PointsToVisitor : public DataflowVisitor<struct PointsToInfo> {
public:
    std::map<int, std::set<std::string>> results;
    int mCount = 0;

    PointsToVisitor() {}

    void merge(PointsToInfo * dest, const PointsToInfo & src) override {
        for (auto &pts: src.pointsToSets) {
            auto tempfind = dest->pointsToSets.find(pts.first);
            if (tempfind == dest->pointsToSets.end()) {
                dest->pointsToSets.insert(pts);
            } else {
                tempfind->second.insert(pts.second.begin(), pts.second.end());
            }
        }
    }

    void compDFVal(Instruction *inst, PointsToInfo * dfval) override {
        if (isa<DbgInfoIntrinsic>(inst)) {
            return;
        }

        if (isa<StoreInst>(inst)) {
            if (mCount > 0) {
                mCount--;
                return;
            }
            StoreInst * storeinst = dyn_cast<StoreInst>(inst);
            Value * value = storeinst->getValueOperand();
            Value * pointer = storeinst->getPointerOperand();

            if (isa<ConstantData>(value)) {
                return;
            }

            //令pointer，要么指向value，要么与value指向相同。如果pointer是某个指针的别名，则同时修改这两个指针的指向集
            if (dfval->pointsToSets.find(value) == dfval->pointsToSets.end()) {
                std::set<Value *> values = {value};
                dfval->pointsToSets[pointer] = values;
                if (dfval->aliases.find(pointer) != dfval->aliases.end()) {
                    for (auto ap: dfval->aliases[pointer]) {
                        dfval->pointsToSets[ap] = values;
                    }
                }
            } else {
                dfval->pointsToSets[pointer] = dfval->pointsToSets[value];
                if (dfval->aliases.find(pointer) != dfval->aliases.end()) {
                    for (auto ap: dfval->aliases[pointer]) {
                        dfval->pointsToSets[ap] = dfval->pointsToSets[value];
                    }
                }
            }
        } else if (isa<LoadInst>(inst)) {
            LoadInst * loadinst = dyn_cast<LoadInst>(inst);
            Value * pointer = loadinst->getPointerOperand();
            Value * result = loadinst;

            if (!pointer->getType()->getContainedType(0)->isPointerTy()) {
                return;
            }

            //令result的指向集与pointer相同，同时设为别名
            std::set<Value *> pts = dfval->pointsToSets[pointer];
            dfval->pointsToSets[result] = pts;
            dfval->aliases[result] = {pointer};
        } else if (isa<MemSetInst>(inst)) {
            return;
        } else if (isa<CallInst>(inst)) {
            CallInst * callinst = dyn_cast<CallInst>(inst);
            Value * called = callinst->getCalledOperand();
            unsigned lineno = callinst->getDebugLoc().getLine();
            auto & funcNames = results[lineno];
            if (isa<Function>(called) && called->getName() == "malloc") {
                funcNames.insert("malloc");
                mCount++;
                return;
            }
            // errs() << *dfval << "\n";
            std::set<Function *> functions;
            if (isa<Function>(called)) {
                functions.insert(dyn_cast<Function>(called));
            } else {
                auto alias = dfval->pointsToSets[called];
                for (auto f : alias) {
                    functions.insert(dyn_cast<Function>(f));
                }
            }
            
            for (auto f : functions) {
                funcNames.insert(f->getName());
                PointsToInfo calleeInfo;
                std::map<Value *, Value *> args;
                for (unsigned i = 0; i < callinst->getNumArgOperands(); i++) {
                    Value * callerArg = callinst->getArgOperand(i);
                    if (callerArg->getType()->isPointerTy()) {
                        Value * calleeArg = f->getArg(i);
                        args[callerArg] = calleeArg;
                        if (dfval->pointsToSets.find(callerArg) != dfval->pointsToSets.end()) {
                            calleeInfo.pointsToSets[calleeArg] = dfval->pointsToSets[callerArg];
                        }
                    }
                }
                if (f->getReturnType()->isPointerTy()) {
                    args[callinst] = f;
                }
                PointsToInfo initval;
                PointsToVisitor visitor;
                BasicBlock *targetEntry = &(f->getEntryBlock());
                BasicBlock *targetExit = &(f->back());
                DataflowResult<PointsToInfo>::Type result;
                result[targetEntry].first = calleeInfo;
                compForwardDataflow(f, &visitor, &result, initval);

                PointsToInfo calleeResult = result[targetExit].second;
                for (auto p: args) {
                    if (calleeResult.pointsToSets.find(p.second) != calleeResult.pointsToSets.end()) {
                        if (dfval->pointsToSets.find(p.first) != dfval->pointsToSets.end()) {
                            dfval->pointsToSets[p.first].insert(calleeResult.pointsToSets[p.second].begin(), calleeResult.pointsToSets[p.second].end());
                        } else {
                            dfval->pointsToSets[p.first] = calleeResult.pointsToSets[p.second];
                        }
                    }
                }

                for (auto line: visitor.results) {
                    if (results.find(line.first) == results.end()) {
                        results[line.first] = line.second;
                    } else {
                        results[line.first].insert(line.second.begin(), line.second.end());
                    }
                }
            }

        } else if (isa<ReturnInst>(inst)) {
            ReturnInst * returninst = dyn_cast<ReturnInst>(inst);
            Value * value = returninst->getReturnValue();
            Value * f = returninst->getFunction();
            if (dfval->pointsToSets.find(value) != dfval->pointsToSets.end()) {
                dfval->pointsToSets[f] = dfval->pointsToSets[value];
            } else {
                dfval->pointsToSets[f] = {value};
            }
        } else if (isa<MemCpyInst>(inst)) {
            ;
        } else if (isa<GetElementPtrInst>(inst)) {
            GetElementPtrInst * getelementptrinst = dyn_cast<GetElementPtrInst>(inst);
            Value * pointer = getelementptrinst->getPointerOperand();
            Value * result = getelementptrinst;

            //将result设为pointer的别名，同时同步这两个指针的指向集
            dfval->aliases[result] = {pointer};
            if (dfval->pointsToSets.find(pointer) != dfval->pointsToSets.end()) {
                dfval->pointsToSets[result] = dfval->pointsToSets[pointer];
            }
        }
    }

    void print(raw_ostream &out) const {
        for (auto line: results) {
            out << line.first << " : ";
            int index = 0;
            for (auto func: line.second) {
                if (index == 0) {
                    out << func;
                } else {
                    out << ", " << func;
                }
                index++;
            }
            out << "\n";
        }
    }
};

class PointsTo : public ModulePass {
public:
    static char ID;
    PointsTo() : ModulePass(ID) {} 

    bool runOnModule(Module &M) override {
    #ifdef _DEBUG
        M.print(llvm::errs(), nullptr);
	    llvm::errs()<<"------------------------------\n";
    #endif
        PointsToVisitor visitor;
        DataflowResult<PointsToInfo>::Type result;
        PointsToInfo initval;

        auto F = M.rbegin();
        while (F->isIntrinsic() || F->getName() == "malloc") {
            F++;
        }
    #ifdef _DEBUG
        llvm::errs() << "Analyse function : " << F->getName() << "\n";
    #endif
        compForwardDataflow(&*F, &visitor, &result, initval);
    #ifdef _DEBUG
        printDataflowResult<PointsToInfo>(errs(), result);
    #endif
        visitor.print(errs());
        return false;
    }
};
