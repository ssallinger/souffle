/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2019, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file RAMI.cpp
 *
 * Implementation of RAMI (RamInterpreter).
 *
 ***********************************************************************/

#include "RAMI.h"
#include "BTree.h"
#include "BinaryConstraintOps.h"
#include "FunctorOps.h"
#include "Global.h"
#include "IODirectives.h"
#include "IOSystem.h"
#include "Logger.h"
#include "ParallelUtils.h"
#include "ProfileEvent.h"
#include "RAMIIndex.h"
#include "RAMIInterface.h"
#include "RAMIRecords.h"
#include "RamExpression.h"
#include "RamNode.h"
#include "RamOperation.h"
#include "RamProgram.h"
#include "RamVisitor.h"
#include "ReadStream.h"
#include "SignalHandler.h"
#include "SymbolTable.h"
#include "Util.h"
#include "WriteStream.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <typeinfo>
#include <utility>
#include <ffi.h>

namespace souffle {

/** Evaluate RAM Expression */
RamDomain RAMI::evalExpr(const RamExpression& expr, const RAMIContext& ctxt) {
    class ExpressionEvaluator : public RamVisitor<RamDomain> {
        RAMI& interpreter;
        const RAMIContext& ctxt;

    public:
        ExpressionEvaluator(RAMI& interp, const RAMIContext& ctxt) : interpreter(interp), ctxt(ctxt) {}

        RamDomain visitNumber(const RamNumber& num) override {
            return num.getConstant();
        }

        RamDomain visitTupleElement(const RamTupleElement& access) override {
            return ctxt[access.getTupleId()][access.getElement()];
        }

        RamDomain visitAutoIncrement(const RamAutoIncrement&) override {
            return interpreter.incCounter();
        }

        // intrinsic functors
        RamDomain visitIntrinsicOperator(const RamIntrinsicOperator& op) override {
            const auto& args = op.getArguments();

            switch (op.getOperator()) {
                /** Unary Functor Operators */
                case FunctorOp::ORD:
                    return visit(args[0]);
                case FunctorOp::STRLEN:
                    return interpreter.getSymbolTable().resolve(visit(args[0])).size();
                case FunctorOp::NEG:
                    return -visit(args[0]);
                case FunctorOp::BNOT:
                    return ~visit(args[0]);
                case FunctorOp::LNOT:
                    return !visit(args[0]);
                case FunctorOp::TONUMBER: {
                    RamDomain result = 0;
                    try {
                        result = stord(interpreter.getSymbolTable().resolve(visit(args[0])));
                    } catch (...) {
                        std::cerr << "error: wrong string provided by to_number(\"";
                        std::cerr << interpreter.getSymbolTable().resolve(visit(args[0]));
                        std::cerr << "\") functor.\n";
                        raise(SIGFPE);
                    }
                    return result;
                }
                case FunctorOp::TOSTRING:
                    return interpreter.getSymbolTable().lookup(std::to_string(visit(args[0])));

                /** Binary Functor Operators */
                case FunctorOp::ADD: {
                    return visit(args[0]) + visit(args[1]);
                }
                case FunctorOp::SUB: {
                    return visit(args[0]) - visit(args[1]);
                }
                case FunctorOp::MUL: {
                    return visit(args[0]) * visit(args[1]);
                }
                case FunctorOp::DIV: {
                    return visit(args[0]) / visit(args[1]);
                }
                case FunctorOp::EXP: {
                    return std::pow(visit(args[0]), visit(args[1]));
                }
                case FunctorOp::MOD: {
                    return visit(args[0]) % visit(args[1]);
                }
                case FunctorOp::BAND: {
                    return visit(args[0]) & visit(args[1]);
                }
                case FunctorOp::BOR: {
                    return visit(args[0]) | visit(args[1]);
                }
                case FunctorOp::BXOR: {
                    return visit(args[0]) ^ visit(args[1]);
                }
                case FunctorOp::LAND: {
                    return visit(args[0]) && visit(args[1]);
                }
                case FunctorOp::LOR: {
                    return visit(args[0]) || visit(args[1]);
                }
                case FunctorOp::MAX: {
                    auto result = visit(args[0]);
                    for (size_t i = 1; i < args.size(); i++) {
                        result = std::max(result, visit(args[i]));
                    }
                    return result;
                }
                case FunctorOp::MIN: {
                    auto result = visit(args[0]);
                    for (size_t i = 1; i < args.size(); i++) {
                        result = std::min(result, visit(args[i]));
                    }
                    return result;
                }
                case FunctorOp::CAT: {
                    std::stringstream ss;
                    for (auto& arg : args) {
                        ss << interpreter.getSymbolTable().resolve(visit(arg));
                    }
                    return interpreter.getSymbolTable().lookup(ss.str());
                }

                /** Ternary Functor Operators */
                case FunctorOp::SUBSTR: {
                    auto symbol = visit(args[0]);
                    const std::string& str = interpreter.getSymbolTable().resolve(symbol);
                    auto idx = visit(args[1]);
                    auto len = visit(args[2]);
                    std::string sub_str;
                    try {
                        sub_str = str.substr(idx, len);
                    } catch (...) {
                        std::cerr << "warning: wrong index position provided by substr(\"";
                        std::cerr << str << "\"," << (int32_t)idx << "," << (int32_t)len << ") functor.\n";
                    }
                    return interpreter.getSymbolTable().lookup(sub_str);
                }

                /** Undefined */
                default: {
                    assert(false && "unsupported operator");
                    return 0;
                }
            }
        }

        RamDomain visitUserDefinedOperator(const RamUserDefinedOperator& op) override {
            // get name and type
            const std::string& name = op.getName();
            const std::string& type = op.getType();

            auto fn = reinterpret_cast<void (*)()>(interpreter.getMethodHandle(name));
            if (fn == nullptr) {
                std::cerr << "Cannot find user-defined operator " << name << std::endl;
                exit(1);
            }
            // prepare dynamic call environment
            size_t arity = op.getArgCount();
            ffi_cif cif;
            ffi_type* args[arity];
            void* values[arity];
            RamDomain intVal[arity];
            const char* strVal[arity];
            ffi_arg rc;

            /* Initialize arguments for ffi-call */
            for (size_t i = 0; i < arity; i++) {
                RamDomain arg = visit(op.getArgument(i));
                if (type[i] == 'S') {
                    args[i] = &ffi_type_pointer;
                    strVal[i] = interpreter.getSymbolTable().resolve(arg).c_str();
                    values[i] = &strVal[i];
                } else {
                    args[i] = &ffi_type_uint32;
                    intVal[i] = arg;
                    values[i] = &intVal[i];
                }
            }

            // call external function
            if (type[arity] == 'N') {
                // Initialize for numerical return value
                if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, arity, &ffi_type_uint32, args) != FFI_OK) {
                    std::cerr << "Failed to prepare CIF for user-defined operator ";
                    std::cerr << name << std::endl;
                    exit(1);
                }
            } else {
                // Initialize for string return value
                if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, arity, &ffi_type_pointer, args) != FFI_OK) {
                    std::cerr << "Failed to prepare CIF for user-defined operator ";
                    std::cerr << name << std::endl;
                    exit(1);
                }
            }
            ffi_call(&cif, fn, &rc, values);
            RamDomain result;
            if (type[arity] == 'N') {
                result = ((RamDomain)rc);
            } else {
                result = interpreter.getSymbolTable().lookup(((const char*)rc));
            }

            return result;
        }

        // -- records --
        RamDomain visitPackRecord(const RamPackRecord& pr) override {
            auto values = pr.getArguments();
            auto arity = values.size();
            RamDomain data[arity];
            for (size_t i = 0; i < arity; ++i) {
                data[i] = visit(values[i]);
            }
            return pack(data, arity);
        }

        // -- subroutine argument
        RamDomain visitSubroutineArgument(const RamSubroutineArgument& arg) override {
            return ctxt.getArgument(arg.getArgument());
        }

        // -- safety net --

        RamDomain visitUndefValue(const RamUndefValue& undef) override {
            assert(false && "Compilation error");
            return 0;
        }

        RamDomain visitNode(const RamNode& node) override {
            std::cerr << "Unsupported node type: " << typeid(node).name() << "\n";
            assert(false && "Unsupported Node Type!");
            return 0;
        }
    };

    // create and run evaluator
    return ExpressionEvaluator(*this, ctxt)(expr);
}

/** Evaluate RAM Condition */
bool RAMI::evalCond(const RamCondition& cond, const RAMIContext& ctxt) {
    class ConditionEvaluator : public RamVisitor<bool> {
        RAMI& interpreter;
        const RAMIContext& ctxt;

    public:
        ConditionEvaluator(RAMI& interp, const RAMIContext& ctxt) : interpreter(interp), ctxt(ctxt) {}

        // -- connectors operators --
        bool visitTrue(const RamTrue& ltrue) override {
            return true;
        }

        bool visitFalse(const RamFalse& lfalse) override {
            return false;
        }

        bool visitConjunction(const RamConjunction& conj) override {
            return visit(conj.getLHS()) && visit(conj.getRHS());
        }

        bool visitNegation(const RamNegation& neg) override {
            return !visit(neg.getOperand());
        }

        // -- relation operations --

        bool visitEmptinessCheck(const RamEmptinessCheck& emptiness) override {
            return interpreter.getRelation(emptiness.getRelation()).empty();
        }

        bool visitExistenceCheck(const RamExistenceCheck& exists) override {
            const RAMIRelation& rel = interpreter.getRelation(exists.getRelation());

            // construct the pattern tuple
            auto arity = rel.getArity();
            auto values = exists.getValues();

            if (Global::config().has("profile") && !exists.getRelation().isTemp()) {
                interpreter.reads[exists.getRelation().getName()]++;
            }
            // for total we use the exists test
            if (interpreter.isa->isTotalSignature(&exists)) {
                RamDomain tuple[arity];
                for (size_t i = 0; i < arity; i++) {
                    assert(!isRamUndefValue(values[i]) && "Value in index is undefined");
                    tuple[i] = interpreter.evalExpr(*values[i], ctxt);
                }
                return rel.exists(tuple);
            }

            // for partial we search for lower and upper boundaries
            RamDomain low[arity];
            RamDomain high[arity];
            for (size_t i = 0; i < arity; i++) {
                low[i] =
                        !isRamUndefValue(values[i]) ? interpreter.evalExpr(*values[i], ctxt) : MIN_RAM_DOMAIN;
                high[i] = !isRamUndefValue(values[i]) ? low[i] : MAX_RAM_DOMAIN;
            }

            // obtain index
            auto idx = rel.getIndex(interpreter.isa->getSearchSignature(&exists));
            auto range = idx->lowerUpperBound(low, high);
            return range.first != range.second;  // if there is something => done
        }

        bool visitProvenanceExistenceCheck(const RamProvenanceExistenceCheck& provExists) override {
            const RAMIRelation& rel = interpreter.getRelation(provExists.getRelation());

            // construct the pattern tuple
            auto arity = rel.getArity();
            auto values = provExists.getValues();

            // for partial we search for lower and upper boundaries
            RamDomain low[arity];
            RamDomain high[arity];
            for (size_t i = 0; i < arity - 2; i++) {
                low[i] =
                        !isRamUndefValue(values[i]) ? interpreter.evalExpr(*values[i], ctxt) : MIN_RAM_DOMAIN;
                high[i] = !isRamUndefValue(values[i]) ? low[i] : MAX_RAM_DOMAIN;
            }

            low[arity - 2] = MIN_RAM_DOMAIN;
            low[arity - 1] = MIN_RAM_DOMAIN;
            high[arity - 2] = MAX_RAM_DOMAIN;
            high[arity - 1] = MAX_RAM_DOMAIN;

            // obtain index
            auto idx = rel.getIndex(interpreter.isa->getSearchSignature(&provExists));
            auto range = idx->lowerUpperBound(low, high);
            return range.first != range.second;  // if there is something => done
        }

        // -- comparison operators --
        bool visitConstraint(const RamConstraint& relOp) override {
            RamDomain lhs = interpreter.evalExpr(relOp.getLHS(), ctxt);
            RamDomain rhs = interpreter.evalExpr(relOp.getRHS(), ctxt);
            switch (relOp.getOperator()) {
                case BinaryConstraintOp::EQ:
                    return lhs == rhs;
                case BinaryConstraintOp::NE:
                    return lhs != rhs;
                case BinaryConstraintOp::LT:
                    return lhs < rhs;
                case BinaryConstraintOp::LE:
                    return lhs <= rhs;
                case BinaryConstraintOp::GT:
                    return lhs > rhs;
                case BinaryConstraintOp::GE:
                    return lhs >= rhs;
                case BinaryConstraintOp::MATCH: {
                    RamDomain l = interpreter.evalExpr(relOp.getLHS(), ctxt);
                    RamDomain r = interpreter.evalExpr(relOp.getRHS(), ctxt);
                    const std::string& pattern = interpreter.getSymbolTable().resolve(l);
                    const std::string& text = interpreter.getSymbolTable().resolve(r);
                    bool result = false;
                    try {
                        result = std::regex_match(text, std::regex(pattern));
                    } catch (...) {
                        std::cerr << "warning: wrong pattern provided for match(\"" << pattern << "\",\""
                                  << text << "\").\n";
                    }
                    return result;
                }
                case BinaryConstraintOp::NOT_MATCH: {
                    RamDomain l = interpreter.evalExpr(relOp.getLHS(), ctxt);
                    RamDomain r = interpreter.evalExpr(relOp.getRHS(), ctxt);
                    const std::string& pattern = interpreter.getSymbolTable().resolve(l);
                    const std::string& text = interpreter.getSymbolTable().resolve(r);
                    bool result = false;
                    try {
                        result = !std::regex_match(text, std::regex(pattern));
                    } catch (...) {
                        std::cerr << "warning: wrong pattern provided for !match(\"" << pattern << "\",\""
                                  << text << "\").\n";
                    }
                    return result;
                }
                case BinaryConstraintOp::CONTAINS: {
                    RamDomain l = interpreter.evalExpr(relOp.getLHS(), ctxt);
                    RamDomain r = interpreter.evalExpr(relOp.getRHS(), ctxt);
                    const std::string& pattern = interpreter.getSymbolTable().resolve(l);
                    const std::string& text = interpreter.getSymbolTable().resolve(r);
                    return text.find(pattern) != std::string::npos;
                }
                case BinaryConstraintOp::NOT_CONTAINS: {
                    RamDomain l = interpreter.evalExpr(relOp.getLHS(), ctxt);
                    RamDomain r = interpreter.evalExpr(relOp.getRHS(), ctxt);
                    const std::string& pattern = interpreter.getSymbolTable().resolve(l);
                    const std::string& text = interpreter.getSymbolTable().resolve(r);
                    return text.find(pattern) == std::string::npos;
                }
                default:
                    assert(false && "unsupported operator");
                    return false;
            }
        }

        bool visitNode(const RamNode& node) override {
            std::cerr << "Unsupported node type: " << typeid(node).name() << "\n";
            assert(false && "Unsupported Node Type!");
            return false;
        }
    };

    // run evaluator
    return ConditionEvaluator(*this, ctxt)(cond);
}

/** Evaluate RAM operation */
void RAMI::evalOp(const RamOperation& op, const RAMIContext& args) {
    class OperationEvaluator : public RamVisitor<bool> {
        RAMI& interpreter;
        RAMIContext& ctxt;

    public:
        OperationEvaluator(RAMI& interp, RAMIContext& ctxt) : interpreter(interp), ctxt(ctxt) {}

        // -- Operations -----------------------------

        bool visitNestedOperation(const RamNestedOperation& nested) override {
            return visit(nested.getOperation());
        }

        bool visitTupleOperation(const RamTupleOperation& search) override {
            bool result = visitNestedOperation(search);

            if (Global::config().has("profile") && !search.getProfileText().empty()) {
                interpreter.frequencies[search.getProfileText()][interpreter.getIterationNumber()]++;
            }
            return result;
        }

        bool visitScan(const RamScan& scan) override {
            // get the targeted relation
            const RAMIRelation& rel = interpreter.getRelation(scan.getRelation());

            // use simple iterator
            for (const RamDomain* cur : rel) {
                ctxt[scan.getTupleId()] = cur;
                if (!visitTupleOperation(scan)) {
                    break;
                }
            }
            return true;
        }

        bool visitIndexScan(const RamIndexScan& scan) override {
            // get the targeted relation
            const RAMIRelation& rel = interpreter.getRelation(scan.getRelation());

            // create pattern tuple for range query
            auto arity = rel.getArity();
            RamDomain low[arity];
            RamDomain hig[arity];
            auto pattern = scan.getRangePattern();
            for (size_t i = 0; i < arity; i++) {
                if (!isRamUndefValue(pattern[i])) {
                    low[i] = interpreter.evalExpr(*pattern[i], ctxt);
                    hig[i] = low[i];
                } else {
                    low[i] = MIN_RAM_DOMAIN;
                    hig[i] = MAX_RAM_DOMAIN;
                }
            }

            // obtain index
            auto idx = rel.getIndex(interpreter.isa->getSearchSignature(&scan));

            // get iterator range
            auto range = idx->lowerUpperBound(low, hig);

            // conduct range query
            for (auto ip = range.first; ip != range.second; ++ip) {
                const RamDomain* data = *(ip);
                ctxt[scan.getTupleId()] = data;
                if (!visitTupleOperation(scan)) {
                    break;
                }
            }
            return true;
        }

        bool visitChoice(const RamChoice& choice) override {
            // get the targeted relation
            const RAMIRelation& rel = interpreter.getRelation(choice.getRelation());

            // use simple iterator
            for (const RamDomain* cur : rel) {
                ctxt[choice.getTupleId()] = cur;
                if (interpreter.evalCond(choice.getCondition(), ctxt)) {
                    visitTupleOperation(choice);
                    break;
                }
            }
            return true;
        }

        bool visitIndexChoice(const RamIndexChoice& choice) override {
            // get the targeted relation
            const RAMIRelation& rel = interpreter.getRelation(choice.getRelation());

            // create pattern tuple for range query
            auto arity = rel.getArity();
            RamDomain low[arity];
            RamDomain hig[arity];
            auto pattern = choice.getRangePattern();
            for (size_t i = 0; i < arity; i++) {
                if (!isRamUndefValue(pattern[i])) {
                    low[i] = interpreter.evalExpr(*pattern[i], ctxt);
                    hig[i] = low[i];
                } else {
                    low[i] = MIN_RAM_DOMAIN;
                    hig[i] = MAX_RAM_DOMAIN;
                }
            }

            // obtain index
            auto idx = rel.getIndex(interpreter.isa->getSearchSignature(&choice));

            // get iterator range
            auto range = idx->lowerUpperBound(low, hig);

            // conduct range query
            for (auto ip = range.first; ip != range.second; ++ip) {
                const RamDomain* data = *(ip);
                ctxt[choice.getTupleId()] = data;
                if (interpreter.evalCond(choice.getCondition(), ctxt)) {
                    visitTupleOperation(choice);
                    break;
                }
            }
            return true;
        }

        bool visitUnpackRecord(const RamUnpackRecord& lookup) override {
            // get reference
            RamDomain ref = interpreter.evalExpr(lookup.getExpression(), ctxt);

            // check for null
            if (isNull(ref)) {
                return true;
            }

            // update environment variable
            auto arity = lookup.getArity();
            const RamDomain* tuple = unpack(ref, arity);

            // save reference to temporary value
            ctxt[lookup.getTupleId()] = tuple;

            // run nested part - using base class visitor
            return visitTupleOperation(lookup);
        }

        bool visitAggregate(const RamAggregate& aggregate) override {
            // get the targeted relation
            const RAMIRelation& rel = interpreter.getRelation(aggregate.getRelation());

            // initialize result
            RamDomain res = 0;
            switch (aggregate.getFunction()) {
                case souffle::MIN:
                    res = MAX_RAM_DOMAIN;
                    break;
                case souffle::MAX:
                    res = MIN_RAM_DOMAIN;
                    break;
                case souffle::COUNT:
                    res = 0;
                    break;
                case souffle::SUM:
                    res = 0;
                    break;
            }

            for (const RamDomain* data : rel) {
                ctxt[aggregate.getTupleId()] = data;

                if (!interpreter.evalCond(aggregate.getCondition(), ctxt)) {
                    continue;
                }

                // count is easy
                if (aggregate.getFunction() == souffle::COUNT) {
                    ++res;
                    continue;
                }

                // aggregation is a bit more difficult

                // eval target expression
                RamDomain cur = interpreter.evalExpr(aggregate.getExpression(), ctxt);

                switch (aggregate.getFunction()) {
                    case souffle::MIN:
                        res = std::min(res, cur);
                        break;
                    case souffle::MAX:
                        res = std::max(res, cur);
                        break;
                    case souffle::COUNT:
                        res = 0;
                        break;
                    case souffle::SUM:
                        res += cur;
                        break;
                }
            }

            // write result to environment
            RamDomain tuple[1];
            tuple[0] = res;
            ctxt[aggregate.getTupleId()] = tuple;

            if (aggregate.getFunction() == souffle::MAX && res == MIN_RAM_DOMAIN) {
                // no maximum found
                return true;
            } else if (aggregate.getFunction() == souffle::MIN && res == MAX_RAM_DOMAIN) {
                // no minimum found
                return true;
            } else {
                // run nested part - using base class visitor
                return visitTupleOperation(aggregate);
            }
        }

        bool visitIndexAggregate(const RamIndexAggregate& aggregate) override {
            // get the targeted relation
            const RAMIRelation& rel = interpreter.getRelation(aggregate.getRelation());

            // initialize result
            RamDomain res = 0;
            switch (aggregate.getFunction()) {
                case souffle::MIN:
                    res = MAX_RAM_DOMAIN;
                    break;
                case souffle::MAX:
                    res = MIN_RAM_DOMAIN;
                    break;
                case souffle::COUNT:
                    res = 0;
                    break;
                case souffle::SUM:
                    res = 0;
                    break;
            }

            // init temporary tuple for this level
            auto arity = rel.getArity();

            // get lower and upper boundaries for iteration
            const auto& pattern = aggregate.getRangePattern();
            RamDomain low[arity];
            RamDomain hig[arity];

            for (size_t i = 0; i < arity; i++) {
                if (!isRamUndefValue(pattern[i])) {
                    low[i] = interpreter.evalExpr(*pattern[i], ctxt);
                    hig[i] = low[i];
                } else {
                    low[i] = MIN_RAM_DOMAIN;
                    hig[i] = MAX_RAM_DOMAIN;
                }
            }

            // obtain index
            auto idx = rel.getIndex(interpreter.isa->getSearchSignature(&aggregate));

            // get iterator range
            auto range = idx->lowerUpperBound(low, hig);

            // iterate through values
            for (auto ip = range.first; ip != range.second; ++ip) {
                // link tuple
                const RamDomain* data = *(ip);
                ctxt[aggregate.getTupleId()] = data;

                if (!interpreter.evalCond(aggregate.getCondition(), ctxt)) {
                    continue;
                }

                // count is easy
                if (aggregate.getFunction() == souffle::COUNT) {
                    ++res;
                    continue;
                }

                // aggregation is a bit more difficult

                // eval target expression
                RamDomain cur = interpreter.evalExpr(aggregate.getExpression(), ctxt);

                switch (aggregate.getFunction()) {
                    case souffle::MIN:
                        res = std::min(res, cur);
                        break;
                    case souffle::MAX:
                        res = std::max(res, cur);
                        break;
                    case souffle::COUNT:
                        res = 0;
                        break;
                    case souffle::SUM:
                        res += cur;
                        break;
                }
            }

            // write result to environment
            RamDomain tuple[1];
            tuple[0] = res;
            ctxt[aggregate.getTupleId()] = tuple;

            // run nested part - using base class visitor
            if (aggregate.getFunction() == souffle::MAX && res == MIN_RAM_DOMAIN) {
                // no maximum found
                return true;
            } else if (aggregate.getFunction() == souffle::MIN && res == MAX_RAM_DOMAIN) {
                // no minimum found
                return true;
            } else {
                // run nested part - using base class visitor
                return visitTupleOperation(aggregate);
            }
        }

        bool visitBreak(const RamBreak& breakOp) override {
            // check condition
            if (interpreter.evalCond(breakOp.getCondition(), ctxt)) {
                return false;
            }
            return visitNestedOperation(breakOp);
        }

        bool visitFilter(const RamFilter& filter) override {
            bool result = true;
            // check condition
            if (interpreter.evalCond(filter.getCondition(), ctxt)) {
                // process nested
                result = visitNestedOperation(filter);
            }

            if (Global::config().has("profile") && !filter.getProfileText().empty()) {
                interpreter.frequencies[filter.getProfileText()][interpreter.getIterationNumber()]++;
            }
            return result;
        }

        bool visitProject(const RamProject& project) override {
            // create a tuple of the proper arity (also supports arity 0)
            auto arity = project.getRelation().getArity();
            const auto& values = project.getValues();
            RamDomain tuple[arity];
            for (size_t i = 0; i < arity; i++) {
                assert(values[i]);
                tuple[i] = interpreter.evalExpr(*values[i], ctxt);
            }

            // insert in target relation
            RAMIRelation& rel = interpreter.getRelation(project.getRelation());
            rel.insert(tuple);
            return true;
        }

        // -- return from subroutine --
        bool visitSubroutineReturnValue(const RamSubroutineReturnValue& ret) override {
            for (auto val : ret.getValues()) {
                if (isRamUndefValue(val)) {
                    ctxt.addReturnValue(0, true);
                } else {
                    ctxt.addReturnValue(interpreter.evalExpr(*val, ctxt));
                }
            }
            return true;
        }

        // -- safety net --
        bool visitNode(const RamNode& node) override {
            std::cerr << "Unsupported node type: " << typeid(node).name() << "\n";
            assert(false && "Unsupported Node Type!");
        }
    };

    // create and run interpreter for operations
    RAMIContext ctxt;
    ctxt.setReturnValues(args.getReturnValues());
    ctxt.setReturnErrors(args.getReturnErrors());
    ctxt.setArguments(args.getArguments());
    OperationEvaluator(*this, ctxt).visit(op);
}

/** Evaluate RAM statement */
void RAMI::evalStmt(const RamStatement& stmt, const RAMIContext& args) {
    class StatementEvaluator : public RamVisitor<bool> {
        RAMI& interpreter;
        const RAMIContext& ctxt;

    public:
        StatementEvaluator(RAMI& interp, const RAMIContext& ctxt) : interpreter(interp), ctxt(ctxt) {}

        // -- Statements -----------------------------

        bool visitSequence(const RamSequence& seq) override {
            // process all statements in sequence
            for (const auto& cur : seq.getStatements()) {
                if (!visit(cur)) {
                    return false;
                }
            }

            // all processed successfully
            return true;
        }

        bool visitParallel(const RamParallel& parallel) override {
            // get statements to be processed in parallel
            const auto& stmts = parallel.getStatements();

            // special case: empty
            if (stmts.empty()) {
                return true;
            }

            // special handling for a single child
            if (stmts.size() == 1) {
                return visit(stmts[0]);
            }

            // parallel execution
            bool cond = true;
#pragma omp parallel for reduction(&& : cond)
            for (size_t i = 0; i < stmts.size(); i++) {  // NOLINT (modernize-loop-convert)
                cond = cond && visit(stmts[i]);
            }
            return cond;
        }

        bool visitLoop(const RamLoop& loop) override {
            interpreter.resetIterationNumber();
            while (visit(loop.getBody())) {
                interpreter.incIterationNumber();
            }
            interpreter.resetIterationNumber();
            return true;
        }

        bool visitExit(const RamExit& exit) override {
            return !interpreter.evalCond(exit.getCondition(), ctxt);
        }

        bool visitLogRelationTimer(const RamLogRelationTimer& timer) override {
            const RAMIRelation& rel = interpreter.getRelation(timer.getRelation());
            Logger logger(timer.getMessage().c_str(), interpreter.getIterationNumber(),
                    std::bind(&RAMIRelation::size, &rel));
            return visit(timer.getStatement());
        }

        bool visitLogTimer(const RamLogTimer& timer) override {
            Logger logger(timer.getMessage().c_str(), interpreter.getIterationNumber());
            return visit(timer.getStatement());
        }

        bool visitDebugInfo(const RamDebugInfo& dbg) override {
            SignalHandler::instance()->setMsg(dbg.getMessage().c_str());
            return visit(dbg.getStatement());
        }

        bool visitStratum(const RamStratum& stratum) override {
            // TODO (lyndonhenry): should enable strata as subprograms for interpreter here

            // Record relations created in each stratum
            if (Global::config().has("profile")) {
                std::map<std::string, size_t> relNames;
                visitDepthFirst(stratum, [&](const RamCreate& create) {
                    relNames[create.getRelation().getName()] = create.getRelation().getArity();
                });
                for (const auto& cur : relNames) {
                    // Skip temporary relations, marked with '@'
                    if (cur.first[0] == '@') {
                        continue;
                    }
                    ProfileEventSingleton::instance().makeStratumRecord(
                            stratum.getIndex(), "relation", cur.first, "arity", std::to_string(cur.second));
                }
            }
            return visit(stratum.getBody());
        }

        bool visitCreate(const RamCreate& create) override {
            interpreter.createRelation(
                    create.getRelation(), &(interpreter.isa->getIndexes(create.getRelation())));
            return true;
        }

        bool visitClear(const RamClear& clear) override {
            RAMIRelation& rel = interpreter.getRelation(clear.getRelation());
            rel.purge();
            return true;
        }

        bool visitDrop(const RamDrop& drop) override {
            interpreter.dropRelation(drop.getRelation());
            return true;
        }

        bool visitLogSize(const RamLogSize& size) override {
            const RAMIRelation& rel = interpreter.getRelation(size.getRelation());
            ProfileEventSingleton::instance().makeQuantityEvent(
                    size.getMessage(), rel.size(), interpreter.getIterationNumber());
            return true;
        }

        bool visitLoad(const RamLoad& load) override {
            for (IODirectives ioDirectives : load.getIODirectives()) {
                try {
                    RAMIRelation& relation = interpreter.getRelation(load.getRelation());
                    std::vector<bool> symbolMask;
                    for (auto& cur : load.getRelation().getAttributeTypeQualifiers()) {
                        symbolMask.push_back(cur[0] == 's');
                    }
                    IOSystem::getInstance()
                            .getReader(symbolMask, interpreter.getSymbolTable(), ioDirectives,
                                    Global::config().has("provenance"))
                            ->readAll(relation);
                } catch (std::exception& e) {
                    std::cerr << "Error loading data: " << e.what() << "\n";
                }
            }
            return true;
        }
        bool visitStore(const RamStore& store) override {
            for (IODirectives ioDirectives : store.getIODirectives()) {
                try {
                    std::vector<bool> symbolMask;
                    for (auto& cur : store.getRelation().getAttributeTypeQualifiers()) {
                        symbolMask.push_back(cur[0] == 's');
                    }
                    IOSystem::getInstance()
                            .getWriter(symbolMask, interpreter.getSymbolTable(), ioDirectives,
                                    Global::config().has("provenance"))
                            ->writeAll(interpreter.getRelation(store.getRelation()));
                } catch (std::exception& e) {
                    std::cerr << e.what();
                    exit(1);
                }
            }
            return true;
        }

        bool visitFact(const RamFact& fact) override {
            auto arity = fact.getRelation().getArity();
            RamDomain tuple[arity];
            auto values = fact.getValues();

            for (size_t i = 0; i < arity; ++i) {
                tuple[i] = interpreter.evalExpr(*values[i], ctxt);
            }

            interpreter.getRelation(fact.getRelation()).insert(tuple);
            return true;
        }

        bool visitQuery(const RamQuery& query) override {
            interpreter.evalOp(query.getOperation(), ctxt);
            return true;
        }

        bool visitMerge(const RamMerge& merge) override {
            // get involved relation
            RAMIRelation& src = interpreter.getRelation(merge.getSourceRelation());
            RAMIRelation& trg = interpreter.getRelation(merge.getTargetRelation());

            if (dynamic_cast<RAMIEqRelation*>(&trg)) {
                // expand src with the new knowledge generated by insertion.
                src.extend(trg);
            }
            // merge in all elements
            trg.insert(src);

            // done
            return true;
        }

        bool visitSwap(const RamSwap& swap) override {
            interpreter.swapRelation(swap.getFirstRelation(), swap.getSecondRelation());
            return true;
        }

        // -- safety net --

        bool visitNode(const RamNode& node) override {
            auto lease = getOutputLock().acquire();
            (void)lease;
            std::cerr << "Unsupported node type: " << typeid(node).name() << "\n";
            assert(false && "Unsupported Node Type!");
            return false;
        }
    };

    // create and run interpreter for operations
    RAMIContext ctxt;
    ctxt.setReturnValues(args.getReturnValues());
    ctxt.setReturnErrors(args.getReturnErrors());
    ctxt.setArguments(args.getArguments());
    StatementEvaluator(*this, ctxt).visit(stmt);

    // create and run interpreter for statements
    // StatementEvaluator(*this).visit(stmt);
}

/** Execute main program of a translation unit */
void RAMI::executeMain() {
    SignalHandler::instance()->set();
    if (Global::config().has("verbose")) {
        SignalHandler::instance()->enableLogging();
    }
    const RamStatement& main = *translationUnit.getProgram()->getMain();

    if (!Global::config().has("profile")) {
        evalStmt(main);
    } else {
        ProfileEventSingleton::instance().setOutputFile(Global::config().get("profile"));
        // Prepare the frequency table for threaded use
        visitDepthFirst(main, [&](const RamTupleOperation& node) {
            if (!node.getProfileText().empty()) {
                frequencies.emplace(node.getProfileText(), std::map<size_t, size_t>());
            }
        });
        // Enable profiling for execution of main
        ProfileEventSingleton::instance().startTimer();
        ProfileEventSingleton::instance().makeTimeEvent("@time;starttime");
        // Store configuration
        for (const auto& cur : Global::config().data()) {
            ProfileEventSingleton::instance().makeConfigRecord(cur.first, cur.second);
        }
        // Store count of relations
        size_t relationCount = 0;
        visitDepthFirst(main, [&](const RamCreate& create) {
            if (create.getRelation().getName()[0] != '@') {
                ++relationCount;
                reads[create.getRelation().getName()] = 0;
            }
        });
        ProfileEventSingleton::instance().makeConfigRecord("relationCount", std::to_string(relationCount));

        // Store count of rules
        size_t ruleCount = 0;
        visitDepthFirst(main, [&](const RamQuery& rule) { ++ruleCount; });
        ProfileEventSingleton::instance().makeConfigRecord("ruleCount", std::to_string(ruleCount));

        evalStmt(main);
        ProfileEventSingleton::instance().stopTimer();
        for (auto const& cur : frequencies) {
            for (auto const& iter : cur.second) {
                ProfileEventSingleton::instance().makeQuantityEvent(cur.first, iter.second, iter.first);
            }
        }
        for (auto const& cur : reads) {
            ProfileEventSingleton::instance().makeQuantityEvent(
                    "@relation-reads;" + cur.first, cur.second, 0);
        }
    }
    SignalHandler::instance()->reset();
}

/** Execute subroutine */
void RAMI::executeSubroutine(const std::string& name, const std::vector<RamDomain>& arguments,
        std::vector<RamDomain>& returnValues, std::vector<bool>& returnErrors) {
    RAMIContext ctxt;
    ctxt.setReturnValues(returnValues);
    ctxt.setReturnErrors(returnErrors);
    ctxt.setArguments(arguments);
    const RamStatement& stmt = translationUnit.getProgram()->getSubroutine(name);

    // run subroutine
    // const RamOperation& op = static_cast<const RamQuery&>(stmt).getOperation();
    evalStmt(stmt, ctxt);
}

}  // end of namespace souffle
