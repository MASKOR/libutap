// -*- mode: C++; c-file-style: "stroustrup"; c-basic-offset: 4; indent-tabs-mode: nil; -*-

/* libutap - Uppaal Timed Automata Parser.
   Copyright (C) 2002-2006 Uppsala University and Aalborg University.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License
   as published by the Free Software Foundation; either version 2.1 of
   the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
   USA
*/

#include "utap/utap.h"
#include "utap/typechecker.h"
#include "utap/systembuilder.h"

#include <sstream>
#include <list>
#include <stdexcept>
#include <cmath>
#include <cstdio>
#include <cassert>
#include <boost/tuple/tuple.hpp>

using std::exception;
using std::set;
using std::pair;
using std::make_pair;
using std::max;
using std::min;
using std::map;
using std::vector;
using std::list;

using boost::tie;

using namespace UTAP;
using namespace Constants;

/* The following are simple helper functions for testing the type of
 * expressions.
 */
static bool isCost(expression_t expr)
{
    return expr.getType().is(COST);
}

static bool isVoid(expression_t expr)
{
    return expr.getType().isVoid();
}

static bool isDouble(expression_t expr)
{
    return expr.getType().isDouble();
}

// static bool isScalar(expression_t expr)
// {
//     return expr.getType().isScalar();
// }

static bool isInteger(expression_t expr)
{
    return expr.getType().isInteger();
}

static bool isBound(expression_t expr)
{
    return expr.getType().isInteger() || expr.getType().isDouble();
}

static bool isIntegral(expression_t expr)
{
    return expr.getType().isIntegral();
}

static bool isClock(expression_t expr)
{
    return expr.getType().isClock();
}

static bool isDiff(expression_t expr)
{
    return expr.getType().isDiff();
}

static bool isDoubleValue(expression_t expr)
{
    return isDouble(expr) || isClock(expr) || isDiff(expr);
}

static bool isNumber(expression_t expr)
{
    return isDoubleValue(expr) || isIntegral(expr);
}

static bool isConstantInteger(expression_t expr)
{
    return expr.getKind() == CONSTANT && isInteger(expr);
}

static bool isConstantDouble(expression_t expr)
{
    return expr.getKind() == CONSTANT && isDouble(expr);
}

static bool isInvariant(expression_t expr)
{
    return expr.getType().isInvariant();
}

static bool isGuard(expression_t expr)
{
    return expr.getType().isGuard();
}

#ifdef ENABLE_PROB
static bool isProbability(expression_t expr)
{
    return expr.getType().isProbability();
}
#endif

static bool isConstraint(expression_t expr)
{
    return expr.getType().isConstraint();
}

static bool isFormula(expression_t expr)
{
    return expr.getType().isFormula();
}

static bool isListOfFormulas(expression_t expr)
{
    if (expr.getKind() != LIST)
    {
        return false;
    }

    for(uint32_t i = 0; i < expr.getSize(); ++i)
    {
        if (!expr[i].getType().isFormula())
        {
            return false;
        }
    }

    return true;
}

static bool hasStrictLowerBound(expression_t expr)
{
    for(size_t i = 0; i < expr.getSize(); ++i)
    {
        if (hasStrictLowerBound(expr[i]))
        {
            return true;
        }
    }

    switch(expr.getKind())
    {
    case LT: // int < clock
        if (isIntegral(expr[0]) && isClock(expr[1]))
        {
            return true;
        }
        break;
    case GT: // clock > int
        if (isClock(expr[0]) && isIntegral(expr[1]))
        {
            return true;
        }
        break;

    default: ;
    }

    return false;
}

static bool hasStrictUpperBound(expression_t expr)
{
    for(size_t i = 0; i < expr.getSize(); ++i)
    {
        if (hasStrictUpperBound(expr[i]))
        {
            return true;
        }
    }

    switch(expr.getKind())
    {
    case GT: // int > clock
        if (isIntegral(expr[0]) && isClock(expr[1]))
        {
            return true;
        }
        break;
    case LT: // clock < int
        if (isClock(expr[0]) && isIntegral(expr[1]))
        {
            return true;
        }
        break;

    default: ;
    }

    return false;
}

/**
 * Returns true iff type is a valid invariant. A valid invariant is
 * either an invariant expression or an integer expression.
 */
static bool isInvariantWR(expression_t expr)
{
    return isInvariant(expr) || (expr.getType().is(INVARIANT_WR));
}

/**
 * Returns true if values of this type can be assigned. This is the
 * case for integers, booleans, clocks, cost, scalars and arrays and
 * records of these. E.g. channels and processes are not assignable.
 */
static bool isAssignable(type_t type)
{
    switch (type.getKind())
    {
    case Constants::INT:
    case Constants::BOOL:
    case Constants::DOUBLE:
    case Constants::CLOCK:
    case Constants::COST:
    case Constants::SCALAR:
        return true;

    case ARRAY:
        return isAssignable(type[0]);

    case RECORD:
        for (size_t i = 0; i < type.size(); i++)
        {
            if (!isAssignable(type[i]))
            {
                return false;
            }
        }
        return true;

    default:
        return type.size() > 0 && isAssignable(type[0]);
    }
}

///////////////////////////////////////////////////////////////////////////

void CompileTimeComputableValues::visitVariable(variable_t &variable)
{
    if (variable.uid.getType().isConstant())
    {
        variables.insert(variable.uid);
    }
}

void CompileTimeComputableValues::visitInstance(instance_t &temp)
{
    frame_t parameters = temp.parameters;
    for (uint32_t i = 0; i < parameters.getSize(); i++)
    {
        type_t type = parameters[i].getType();
        if (!type.is(REF) && type.isConstant() && !type.isDouble())
        {
            variables.insert(parameters[i]);
        }
    }
}

bool CompileTimeComputableValues::contains(symbol_t symbol) const
{
    return (variables.find(symbol) != variables.end());
}

///////////////////////////////////////////////////////////////////////////

class RateDecomposer
{
public:
    RateDecomposer()
        : invariant(expression_t::createConstant(1)),
          hasStrictInvariant(false), hasClockRates(false),
          countCostRates(0) {}

    expression_t costRate;
    expression_t invariant;
    bool hasStrictInvariant, hasClockRates;
    size_t countCostRates;

    void decompose(expression_t,bool inforall = false);
};

void RateDecomposer::decompose(expression_t expr, bool inforall)
{
    assert(isInvariantWR(expr));

    if (isInvariant(expr))
    {
        if (expr.getKind() == Constants::LT)
        {
            hasStrictInvariant = true; // Strict upper bounds only.
        }
        if (!inforall)
        {
            invariant = invariant.empty()
                ? expr
                : invariant = expression_t::createBinary(
                    AND, invariant, expr,
                    expr.getPosition(),
                    type_t::createPrimitive(INVARIANT));
        }
    }
    else if (expr.getKind() == AND)
    {
        decompose(expr[0], inforall);
        decompose(expr[1], inforall);
    }
    else if (expr.getKind() == EQ)
    {
        expression_t left, right;
        assert((expr[0].getType().getKind() == RATE)
               ^ (expr[1].getType().getKind() == RATE));

        if (expr[0].getType().getKind() == RATE)
        {
            left = expr[0][0];
            right = expr[1];
        }
        else
        {
            left = expr[1][0];
            right = expr[0];
        }

        if (isCost(left))
        {
            costRate = right;
            countCostRates++;
        }
        else
        {
            hasClockRates = true;
            if (!inforall)
            {
                invariant = invariant.empty()
                    ? expr
                    : expression_t::createBinary(
                        AND, invariant, expr,
                        expr.getPosition(),
                        type_t::createPrimitive(INVARIANT_WR));
            }
        }
    }
    else
    {
        assert(expr.getType().is(INVARIANT_WR));
        assert(expr.getKind() == FORALL);
        // Enter the forall to look for clock rates but don't
        // record them, rather the forall expression.
        decompose(expr[1], true);
        invariant = invariant.empty()
            ? expr
            : invariant = expression_t::createBinary(
                AND, invariant, expr,
                expr.getPosition(),
                type_t::createPrimitive(INVARIANT_WR));
    }
}

///////////////////////////////////////////////////////////////////////////

TypeChecker::TypeChecker(
    TimedAutomataSystem *_system, bool refinement)
    : system{_system}, refinementWarnings{refinement}
{
    system->accept(compileTimeComputableValues);
    checkExpression(system->getBeforeUpdate());
    checkExpression(system->getAfterUpdate());
}

template<class T>
void TypeChecker::handleWarning(const T& expr, const std::string& msg)
{
    system->addWarning(expr.getPosition(), msg, "(typechecking)");
}

template<class T>
void TypeChecker::handleError(const T& expr, const std::string& msg)
{
    system->addError(expr.getPosition(), msg, "(typechecking)");
}

/**
 * This method issues warnings for expressions, which do not change
 * any variables. It is expected to be called for all expressions
 * whose value is ignored. Unless such an expression has some kind
 * of side-effect, it does not have any purpose.
 *
 * Notice that in contrast to the regular side-effect analysis,
 * this function accepts modifications of local variables as a
 * side-effect.
 */
void TypeChecker::checkIgnoredValue(expression_t expr)
{
    if (expr.getKind()!=EXIT && !expr.changesAnyVariable())
    {
        handleWarning(expr, "$Expression_does_not_have_any_effect");
    }
    else if (expr.getKind() == COMMA)
    {
        checkIgnoredValue(expr[1]);
    }
}

bool TypeChecker::isCompileTimeComputable(expression_t expr) const
{
    /* An expression is compile time computable if all identifers it
     * could possibly access during an evaluation are compile time
     * computable (i.e. their value is known at compile time).
     *
     * FIXME: We could maybe refine this to actually include function
     * local variables with compile time computable initialisers. This
     * would increase the class of models we accept while also getting
     * rid of the compileTimeComputableValues object.
     */
    set<symbol_t> reads;
    expr.collectPossibleReads(reads, true);
    for (set<symbol_t>::iterator i = reads.begin(); i != reads.end(); i++)
    {
        if (*i == symbol_t() ||
            (!i->getType().isFunction()
             && !compileTimeComputableValues.contains(*i)))
        {
            return false;
        }
    }
    return true;
}

// static bool contains(frame_t frame, symbol_t symbol)
// {
//     for (size_t i = 0; i < frame.getSize(); i++)
//     {
//         if (frame[i] == symbol)
//         {
//             return true;
//         }
//     }
//     return false;
// }

/**
 * Check that the type is valid, i.e.:
 *
 * - all expressions such as array sizes, integer ranges, etc. are
 *   type correct,
 *
 * - only allowed prefixes are used (e.g. no urgent integers).
 *
 * - array sizes and integer bounds are compile time computable.
 *
 * If \a initialisable is true, then this method also checks that \a
 * type is initialisable.
 */
void TypeChecker::checkType(type_t type, bool initialisable, bool inStruct)
{
    expression_t l, u;
    type_t size;
    frame_t frame;

    switch (type.getKind())
    {
    case LABEL:
        checkType(type[0], initialisable, inStruct);
        break;

    case URGENT:
        if (!type.isLocation() && !type.isChannel())
        {
            handleError(type, "$Prefix_urgent_only_allowed_for_locations_and_channels");
        }
        checkType(type[0], initialisable, inStruct);
        break;

    case BROADCAST:
        if (!type.isChannel())
        {
            handleError(type, "$Prefix_broadcast_only_allowed_for_channels");
        }
        checkType(type[0], initialisable, inStruct);
        break;

    case COMMITTED:
        if (!type.isLocation())
        {
            handleError(type, "$Prefix_committed_only_allowed_for_locations");
        }
        checkType(type[0], initialisable, inStruct);
        break;

    case HYBRID:
        if (!type.isClock() && !(type.isArray() && type.stripArray().isClock()))
        {
            handleError(type, "$Prefix_hybrid_only_allowed_for_clocks");
        }
        checkType(type[0], initialisable, inStruct);
        break;

    case CONSTANT:
        if (type.isClock())
        {
            handleError(type, "$Prefix_const_not_allowed_for_clocks");
        }
        checkType(type[0], true, inStruct);
        break;

    case SYSTEM_META:
        if (type.isClock())
        {
            handleError(type, "$Prefix_meta_not_allowed_for_clocks");
        }
        checkType(type[0], true, inStruct);
        break;

    case REF:
        if (!type.isIntegral() && !type.isArray() && !type.isRecord()
            && !type.isChannel() && !type.isClock() && !type.isScalar()
            && !type.isDouble())
        {
            handleError(type, "$Reference_to_this_type_not_allowed");
        }
        checkType(type[0], initialisable, inStruct);
        break;

    case RANGE:
        if (!type.isInteger() && !type.isScalar())
        {
            handleError(type, "$Range_over_this_type_not_allowed");
        }
        tie(l, u) = type.getRange();
        if (checkExpression(l))
        {
            if (!isInteger(l))
            {
                handleError(l, "$Integer_expected");
            }
            if (!isCompileTimeComputable(l))
            {
                handleError(l, "$Must_be_computable_at_compile_time");
            }
        }
        if (checkExpression(u))
        {
            if (!isInteger(u))
            {
                handleError(u, "$Integer_expected");
            }
            if (!isCompileTimeComputable(u))
            {
                handleError(u, "$Must_be_computable_at_compile_time");
            }
        }
        break;

    case ARRAY:
        size = type.getArraySize();
        if (!size.is(RANGE))
        {
            handleError(type, "$Invalid_array_size");
        }
        else
        {
            checkType(size);
        }
        checkType(type[0], initialisable, inStruct);
        break;

    case RECORD:
        for (size_t i = 0; i < type.size(); i++)
        {
            checkType(type.getSub(i), true, true);
        }
        break;

    case Constants::DOUBLE:
        if (inStruct)
        {
            handleError(type, "$This_type_cannot_be_declared_inside_a_struct");
        }
    case Constants::INT:
    case Constants::BOOL:
        break;

    default:
        if (initialisable)
        {
            handleError(type, "$This_type_cannot_be_declared_const_or_meta");
        }
    }
}

void TypeChecker::visitSystemAfter(TimedAutomataSystem* sys)
{
    const std::list<chan_priority_t>& list = sys->getChanPriorities();
    std::list<chan_priority_t>::const_iterator i;
    for (i = list.begin(); i != list.end(); i++)
    {
        bool isDefault = (i->head == expression_t());
        if (!isDefault && checkExpression(i->head))
        {
            expression_t expr = i->head;
            type_t channel = expr.getType();

            // Check that chanElement is a channel, or an array of channels.
            while (channel.isArray())
            {
                channel = channel.getSub();
            }
            if (!channel.isChannel())
            {
                handleError(expr, "$Channel_expected");
            }

            // Check index expressions
            while (expr.getKind() == ARRAY)
            {
                if (!isCompileTimeComputable(expr[1]))
                {
                    handleError(expr[1], "$Must_be_computable_at_compile_time");
                }
                else if (i->head.changesAnyVariable())
                {
                    handleError(expr[1], "$Index_must_be_side-effect_free");
                }
                expr = expr[0];
            }
        }

        chan_priority_t::tail_t::const_iterator j;
        for (j = i->tail.begin(); j != i->tail.end(); ++j)
        {
            bool isDefault = (j->second == expression_t());
            if (!isDefault && checkExpression(j->second))
            {
                expression_t expr = j->second;
                type_t channel = expr.getType();

                // Check that chanElement is a channel, or an array of channels.
                while (channel.isArray())
                {
                    channel = channel.getSub();
                }
                if (!channel.isChannel())
                {
                    handleError(expr, "$Channel_expected");
                }

                // Check index expressions
                while (expr.getKind() == ARRAY)
                {
                    if (!isCompileTimeComputable(expr[1]))
                    {
                        handleError(expr[1], "$Must_be_computable_at_compile_time");
                    }
                    else if (j->second.changesAnyVariable())
                    {
                        handleError(expr[1], "$Index_must_be_side-effect_free");
                    }
                    expr = expr[0];
                }
            }
        }
    }
}

void TypeChecker::visitHybridClock(expression_t e)
{
    if (checkExpression(e))
    {
        if (!isClock(e))
        {
            handleError(e, "$Clock_expected");
        }
        else if (e.changesAnyVariable())
        {
            handleError(e, "$Index_must_be_side-effect_free");
        }
        // Should be a check to identify the clock at compile time.
        // Problematic now. Same issue for inf & sup.
    }
}

void TypeChecker::visitIODecl(iodecl_t &iodecl)
{
    for(vector<expression_t>::iterator i = iodecl.param.begin();
        i != iodecl.param.end(); ++i)
    {
        expression_t e = *i;
        if (checkExpression(e))
        {
            if (!isInteger(e))
            {
                handleError(e, "$Integer_expected");
            }
            else if (!isCompileTimeComputable(e))
            {
                handleError(e, "$Must_be_computable_at_compile_time");
            }
            else if (e.changesAnyVariable())
            {
                handleError(e, "$Index_must_be_side-effect_free");
            }
        }
    }

    if (syncUsed == sync_use_t::unused)
    {
        if (!iodecl.inputs.empty() || !iodecl.outputs.empty())
        {
            syncUsed = sync_use_t::io;
        }
        else if (!iodecl.csp.empty())
        {
            syncUsed = sync_use_t::csp;
        }
    }
    if (syncUsed == sync_use_t::io)
    {
        if (!iodecl.csp.empty())
        {
            syncError = true;
        }
    }
    else if (syncUsed == sync_use_t::csp)
    {
        if (!iodecl.inputs.empty() || !iodecl.outputs.empty())
        {
            syncError = true;
        }
    }
    if (syncError)
    {
        handleError(iodecl.csp.front(), "$CSP_and_IO_synchronisations_cannot_be_mixed");
    }

    system->setSyncUsed(syncUsed);

    for(list<expression_t>::iterator i = iodecl.inputs.begin();
        i != iodecl.inputs.end(); ++i)
    {
        if (checkExpression(*i))
        {
            type_t channel = i->getType();
            expression_t expr = *i;

            // Check that chanElement is a channel, or an array of channels.
            while (channel.isArray())
            {
                channel = channel.getSub();
            }
            if (!channel.isChannel())
            {
                handleError(expr, "$Channel_expected");
            }

            // Check index expressions
            while (expr.getKind() == ARRAY)
            {
                if (!isCompileTimeComputable(expr[1]))
                {
                    handleError(expr[1], "$Must_be_computable_at_compile_time");
                }
                else if (i->changesAnyVariable())
                {
                    handleError(expr[1], "$Index_must_be_side-effect_free");
                }
                expr = expr[0];
            }
        }
    }

    for(list<expression_t>::iterator i = iodecl.outputs.begin();
        i != iodecl.outputs.end(); ++i)
    {
        if (checkExpression(*i))
        {
            type_t channel = i->getType();
            expression_t expr = *i;

            // Check that chanElement is a channel, or an array of channels.
            while (channel.isArray())
            {
                channel = channel.getSub();
            }
            if (!channel.isChannel())
            {
                handleError(expr, "$Channel_expected");
            }

            // Check index expressions
            while (expr.getKind() == ARRAY)
            {
                if (!isCompileTimeComputable(expr[1]))
                {
                    handleError(expr[1], "$Must_be_computable_at_compile_time");
                }
                else if (i->changesAnyVariable())
                {
                    handleError(expr[1], "$Index_must_be_side-effect_free");
                }
                expr = expr[0];
            }
        }
    }
}

void TypeChecker::visitProcess(instance_t &process)
{
    for (size_t i = 0; i < process.unbound; i++)
    {
        /* Unbound parameters of processes must be either scalars or
         * bounded integers.
         */
        symbol_t parameter = process.parameters[i];
        type_t type = parameter.getType();
        if (!(type.isScalar() || type.isRange()) || type.is(REF))
        {
            handleError(type, "$Free_process_parameters_must_be_a_bounded_integer_or_a_scalar");
        }

        /* Unbound parameters must not be used either directly or
         * indirectly in any array size declarations. I.e. they must
         * not be restricted.
         */
        if (process.restricted.find(parameter) != process.restricted.end())
        {
            handleError(type, "$Free_process_parameters_must_not_be_used_directly_or_indirectly_in_an_array_declaration_or_select_expression");
        }
    }
}

void TypeChecker::visitVariable(variable_t &variable)
{
    SystemVisitor::visitVariable(variable);

    checkType(variable.uid.getType());
    if (variable.expr.isDynamic() || variable.expr.hasDynamicSub ())
    {
        handleError (variable.expr,"Dynamic constructions cannot be used as initialisers");
    }
    else if (!variable.expr.empty() && checkExpression(variable.expr))
    {
        if (!isCompileTimeComputable(variable.expr))
        {
            handleError(variable.expr, "$Must_be_computable_at_compile_time");
        }
        else if (variable.expr.changesAnyVariable())
        {
            handleError(variable.expr, "$Initialiser_must_be_side-effect_free");
        }
        else
        {
            variable.expr = checkInitialiser(variable.uid.getType(), variable.expr);
        }
    }
}

void TypeChecker::visitState(state_t &state)
{
    SystemVisitor::visitState(state);

    if (!state.invariant.empty())
    {
        if (checkExpression(state.invariant))
        {
            if (!isInvariantWR(state.invariant))
            {
                std::string s = "$Expression_of_type ";
                s += state.invariant.getType().toString();
                s += " $cannot_be_used_as_an_invariant";
                handleError(state.invariant, s);
            }
            else if (state.invariant.changesAnyVariable())
            {
                handleError(state.invariant, "$Invariant_must_be_side-effect_free");
            }
            else
            {
                RateDecomposer decomposer;
                decomposer.decompose(state.invariant);
                state.invariant = decomposer.invariant;
                state.costRate = decomposer.costRate;
                if (decomposer.countCostRates > 1)
                {
                    handleError(state.invariant, "$Only_one_cost_rate_is_allowed");
                }
                if (decomposer.hasClockRates)
                {
                    system->recordStopWatch();
                }
                if (decomposer.hasStrictInvariant)
                {
                    system->recordStrictInvariant();
#if ENABLE_TIGA
                    handleWarning(state.invariant, "$Strict_invariant");
#endif
                }
            }
        }
    }
    if (!state.exponentialRate.empty())
    {
        if (checkExpression(state.exponentialRate))
        {
            if (!isIntegral(state.exponentialRate) &&
                state.exponentialRate.getKind() != FRACTION &&
                !state.exponentialRate.getType().isDouble())
            {
                handleError(state.exponentialRate, "$Number_expected");
            }
        }
    }
}

void TypeChecker::visitEdge(edge_t &edge)
{
    SystemVisitor::visitEdge(edge);

    // select
    frame_t select = edge.select;
    for (size_t i = 0; i < select.getSize(); i++)
    {
        checkType(select[i].getType());
    }

    // guard
    bool strictBound = false;
    if (!edge.guard.empty())
    {
        if (checkExpression(edge.guard))
        {
            if (!isGuard(edge.guard))
            {
                std::string s = "$Expression_of_type ";
                s += edge.guard.getType().toString();
                s += " $cannot_be_used_as_a_guard";
                handleError(edge.guard, s);
            }
            else if (edge.guard.changesAnyVariable())
            {
                handleError(edge.guard, "$Guard_must_be_side-effect_free");
            }
            if (hasStrictLowerBound(edge.guard))
            {
                if (edge.control)
                {
                    system->recordStrictLowerBoundOnControllableEdges();
                }
                strictBound = true;
            }
            if (hasStrictUpperBound(edge.guard))
            {
                strictBound = true;
            }
        }
    }

    // sync
    if (!edge.sync.empty())
    {
        if (checkExpression(edge.sync))
        {
            type_t channel = edge.sync.get(0).getType();
            if (!channel.isChannel())
            {
                handleError(edge.sync.get(0), "$Channel_expected");
            }
            else if (edge.sync.changesAnyVariable())
            {
                handleError(edge.sync,
                            "$Synchronisation_must_be_side-effect_free");
            }
            else
            {
                bool hasClockGuard =
                    !edge.guard.empty() && !isIntegral(edge.guard);
                bool isUrgent = channel.is(URGENT);
                bool receivesBroadcast = channel.is(BROADCAST)
                    && edge.sync.getSync() == SYNC_QUE;

                if (isUrgent && hasClockGuard)
                {
                    system->setUrgentTransition();
                    handleWarning(edge.sync,
                                  "$Clock_guards_are_not_allowed_on_urgent_edges");
                }
                else if (receivesBroadcast && hasClockGuard)
                {
                    system->clockGuardRecvBroadcast();
                    /*
                      This is now allowed, though it is expensive.

                    handleError(edge.sync,
                                "$Clock_guards_are_not_allowed_on_broadcast_receivers");
                    */
                }
                if (receivesBroadcast && edge.guard.isTrue())
                {
                    if (edge.dst == NULL)
                    { // dst is null at least in a case of branchpoint
                        handleWarning(edge.sync,
                                      "SMC requires input edges to be deterministic");
                    }
#ifndef NDEBUG
                    else if (!edge.dst->invariant.isTrue())
                    {
                        // This case is not handled correctly by the engine and it is expensive to fix.
                        handleWarning(edge.sync,
                                      "$It_may_be_needed_to_add_a_guard_involving_the_target_invariant");
                    }
                    /*
                      The warning above gives too many false alarms and is therefore disabled.
                      In particular it does not consider the common idiom of clock reset (i.e. guard is irrelevant).
                      Details: the above case may lead to violation of target invariant if unchecked,
                      however the invariant *is* being checked in the engine and halts with
                      "violates model sanity with transition" + proper diagnostics about the transition and location.
                    */
#endif /* NDEBUG */
                }
                if (isUrgent && strictBound)
                {
                    handleWarning(edge.guard,
                                  "$Strict_bounds_on_urgent_edges_may_not_make_sense");
                }
            }

            switch(syncUsed)
            {
            case sync_use_t::unused:
                switch(edge.sync.getSync())
                {
                case SYNC_BANG:
                case SYNC_QUE:
                    syncUsed = sync_use_t::io;
                    break;
                case SYNC_CSP:
                    syncUsed = sync_use_t::csp;
                    break;
                }
                break;
            case sync_use_t::io:
                switch(edge.sync.getSync())
                {
                case SYNC_BANG:
                case SYNC_QUE:
                    // ok
                    break;
                case SYNC_CSP:
                    syncError = true;
                    handleError(edge.sync, "$Assumed_IO_but_found_CSP_synchronization");
                    break;
                }
                break;
            case sync_use_t::csp:
                switch(edge.sync.getSync())
                {
                case SYNC_BANG:
                case SYNC_QUE:
                    syncError = true;
                    handleError(edge.sync, "$Assumed_CSP_but_found_IO_synchronization");
                    break;
                case SYNC_CSP:
                    // ok
                    break;
                }
                break;
            default:
            // nothing
            ;
            }

            if (refinementWarnings)
            {
                if (edge.sync.getSync() == SYNC_BANG)
                {
                    if (edge.control)
                    {
                        handleWarning(edge.sync,
                                      "$Outputs_should_be_uncontrollable_for_refinement_checking");
                    }
                }
                else if (edge.sync.getSync() == SYNC_QUE)
                {
                    if (!edge.control)
                    {
                        handleWarning(edge.sync,
                                      "$Inputs_should_be_controllable_for_refinement_checking");
                    }
                }
                else
                {
                    handleWarning(edge.sync,
                                  "$CSP_synchronisations_are_incompatible_with_refinement_checking");
                }
            }
        }
    }

    // assignment
    checkAssignmentExpression(edge.assign);

#ifdef ENABLE_PROB
    // probability
    if (!edge.prob.empty())
    {
        if (checkExpression(edge.prob))
        {
            if (!isProbability(edge.prob))
            {
                std::string s = "$Expression_of_type ";
                s += edge.prob.getType().toString();
                s += " $cannot_be_used_as_a_probability";
                handleError(edge.prob, s);
            }
            else if (edge.prob.changesAnyVariable())
            {
                handleError(edge.prob, "$Probability_must_be_side-effect_free");
            }
        }
    }
#endif
}

void TypeChecker::visitInstanceLine(instanceLine_t &instance) {
    SystemVisitor::visitInstanceLine(instance);
}

void TypeChecker::visitMessage(message_t &message) {
    SystemVisitor::visitMessage(message);

    if (!message.label.empty())
    {
        if (checkExpression(message.label))
        {
            type_t channel = message.label.get(0).getType();
            if (!channel.isChannel())
            {
                handleError(message.label.get(0), "$Channel_expected");
            }
            else if (message.label.changesAnyVariable())
            {
                handleError(message.label,
                        "$Message_must_be_side-effect_free");
            }
        }
    }
}
void TypeChecker::visitCondition(condition_t &condition) {
    SystemVisitor::visitCondition(condition);
    if (!condition.label.empty())
    {
        if (checkExpression(condition.label))
        {
            if (!isGuard(condition.label))
            {
                std::string s = "$Expression_of_type ";
                s += condition.label.getType().toString();
                s += " $cannot_be_used_as_a_condition";
                handleError(condition.label, s);
            }
            else if (condition.label.changesAnyVariable())
            {
                handleError(condition.label, "$Condition_must_be_side-effect_free");
            }
        }
    }
}
void TypeChecker::visitUpdate(update_t &update) {
    SystemVisitor::visitUpdate(update);
    if (!update.label.empty())
    {
        checkAssignmentExpression(update.label);
    }
}

void TypeChecker::visitProgressMeasure(progress_t &progress)
{
    checkExpression(progress.guard);
    checkExpression(progress.measure);

    if (!progress.guard.empty() && !isIntegral(progress.guard))
    {
        handleError(progress.guard,
                    "$Progress_guard_must_evaluate_to_a_boolean");
    }

    if (!isIntegral(progress.measure))
    {
        handleError(progress.measure,
                    "$Progress_measure_must_evaluate_to_a_value");
    }
}

void TypeChecker::visitGanttChart(gantt_t &gc)
{
    size_t n = gc.parameters.getSize();
    for(size_t i = 0; i < n; ++i)
    {
        checkType(gc.parameters[i].getType());
    }

    std::list<ganttmap_t>::const_iterator first, end = gc.mapping.end();
    for(first = gc.mapping.begin(); first != end; ++first)
    {
        n = (*first).parameters.getSize();
        for(size_t i = 0; i < n; ++i)
        {
            checkType((*first).parameters[i].getType());
        }

        const expression_t &p = (*first).predicate;
        checkExpression(p);
        if (!isIntegral(p) && !isConstraint(p))
        {
            handleError(p, "$Boolean_expected");
        }

        const expression_t &m = (*first).mapping;
        checkExpression(m);
        if (!isIntegral(m))
        {
            handleError(m, "$Integer_expected");
        }
    }
}

void TypeChecker::visitInstance(instance_t &instance)
{
    SystemVisitor::visitInstance(instance);

    /* Check the parameters of the instance.
     */
    type_t type = instance.uid.getType();
    for (size_t i = 0; i < type.size(); i++)
    {
        checkType(type[i]);
    }

    /* Check arguments.
     */
    for (size_t i = type.size(); i < type.size() + instance.arguments; i++)
    {
        symbol_t parameter = instance.parameters[i];
        expression_t argument = instance.mapping[parameter];

        if (!checkExpression(argument))
        {
            continue;
        }

        // For template instantiation, the argument must be side-effect free
        if (argument.changesAnyVariable())
        {
            handleError(argument, "$Argument_must_be_side-effect_free");
            continue;
        }

        // We have three ok cases:
        // - Value parameter with computable argument
        // - Constant reference with computable argument
        // - Reference parameter with unique lhs argument
        // If non of the cases are true, then we generate an error
        bool ref = parameter.getType().is(REF);
        bool constant = parameter.getType().isConstant();
        bool computable = isCompileTimeComputable(argument);

        if ((!ref && !computable)
            || (ref && !constant && !isUniqueReference(argument))
            || (ref && constant && !computable))
        {
            handleError(argument, "$Incompatible_argument");
            continue;
        }

        checkParameterCompatible(parameter.getType(), argument);
    }
}

static bool isGameProperty(expression_t expr)
{
    switch(expr.getKind())
    {
    case CONTROL:
    case SMC_CONTROL:
    case EF_CONTROL:
    case CONTROL_TOPT:
    case PO_CONTROL:
    case CONTROL_TOPT_DEF1:
    case CONTROL_TOPT_DEF2:
    case SIMULATION_LE:
    case SIMULATION_GE:
    case REFINEMENT_LE:
    case REFINEMENT_GE:
    case CONSISTENCY:
    case IMPLEMENTATION:
    case SPECIFICATION:
        return true;
    default:
        return false;
    }
}


static bool hasMITLInQuantifiedSub(expression_t expr)
{
    bool hasIt = (expr.getKind () == MITLFORALL || expr.getKind () == MITLEXISTS);
    if (!hasIt)
    {
        for (uint32_t i = 0; i < expr.getSize(); i++)
        {
            hasIt |= hasMITLInQuantifiedSub (expr.get(i));
        }
    }
    return hasIt;
}

static bool hasSpawnOrExit(expression_t expr) {
    bool hasIt = (expr.getKind () == SPAWN || expr.getKind () == EXIT);
    if (!hasIt)
    {
        for (uint32_t i = 0; i < expr.getSize(); i++)
        {
            hasIt |= hasSpawnOrExit (expr.get(i));
        }
    }
    return hasIt;
}

void TypeChecker::visitProperty(expression_t expr)
{
    if (checkExpression(expr))
    {
        if (expr.changesAnyVariable())
        {
            handleError(expr, "$Property_must_be_side-effect_free");
        }
        if (!((expr.getType().is(TIOGRAPH) && expr.getKind() == CONSISTENCY) ||
              isFormula(expr)))
        {
            handleError(expr, "$Property_must_be_a_valid_formula");
        }
        if (isGameProperty(expr))
        {
            /*
            for (uint32_t i = 0; i < expr.getSize(); i++)
            {
                if (isFormula(expr[i]))
                {
                    for (uint32_t j = 0; j < expr[i].getSize(); j++)
                    {
                        if (!isConstraint(expr[i][j]))
                        {
                            handleError(expr[i][j], "$Nesting_of_path_quantifiers_is_not_allowed");
                        }
                    }
                }
            }
            */
        }
        else if (expr.getKind() != SUP_VAR &&
                 expr.getKind() != INF_VAR &&
                 expr.getKind() != SCENARIO &&
                 expr.getKind() != PROBAMINBOX &&
                 expr.getKind() != PROBAMINDIAMOND &&
                 expr.getKind() != PROBABOX &&
                 expr.getKind() != PROBADIAMOND &&
                 expr.getKind() != PROBAEXP &&
                 expr.getKind() != PROBACMP &&
                 expr.getKind() != SIMULATE &&
                 expr.getKind() != SIMULATEREACH &&
                 expr.getKind() != MITLFORMULA)
        {
            for (uint32_t i = 0; i < expr.getSize(); i++)
            {
                /* No nesting except for constraints */
                if (!isConstraint(expr[i]))
                {
                    handleError(expr[i], "$Nesting_of_path_quantifiers_is_not_allowed");
                }
            }
        }
        if (expr.getKind() == PO_CONTROL)
        {
            /* Observations on clock constraints are limited to be
             * weak for lower bounds and strict for upper bounds.
             */
            checkObservationConstraints(expr);
        }
        if (hasMITLInQuantifiedSub (expr) && expr.getKind () != MITLFORMULA)
        {
            handleError(expr, "MITL inside forall or exists in non-MITL property");

        }
    }
}

/**
 * Checks that \a expr is a valid assignment expression. Errors or
 * warnings are issued via calls to handleError() and
 * handleWarning(). Returns true if no errors were issued, false
 * otherwise.
 *
 * An assignment expression is any:
 *
 *  - expression of an expression statement,
 *
 *  - initialisation or step expression in a for-clause
 *
 *  - expression in the update field of an edge
 *
 *  - expression in the label field of an update (LSC)
 */
bool TypeChecker::checkAssignmentExpression(expression_t expr)
{
    if (!checkExpression(expr))
    {
        return false;
    }

    if (!isAssignable(expr.getType()) && !isVoid(expr))
    {
        handleError(expr, "$Invalid_assignment_expression");
        return false;
    }

    if (expr.getKind() != CONSTANT  || expr.getValue() != 1)
    {
        checkIgnoredValue(expr);
    }

    return true;
}

/** Checks that the expression can be used as a condition (e.g. for if). */
bool TypeChecker::checkConditionalExpressionInFunction(expression_t expr)
{
    if (!isIntegral(expr))
    {
        handleError(expr, "$Boolean_expected");
        return false;
    }
    return true;
}


void TypeChecker::checkObservationConstraints(expression_t expr)
{
    for(size_t i = 0; i < expr.getSize(); ++i)
    {
        checkObservationConstraints(expr[i]);
    }

    bool invalid = false;

    switch(expr.getKind())
    {
    case LT: // int < clock
    case GE: // int >= clock
        invalid = isIntegral(expr[0]) && isClock(expr[1]);
        break;

    case LE: // clock <= int
    case GT: // clock > int
        invalid = isClock(expr[0]) && isIntegral(expr[1]);
        break;

    case EQ:  // clock == int || int == clock
    case NEQ: // clock != int || int != clock
        invalid = (isClock(expr[0]) && isIntegral(expr[1]))
            || (isIntegral(expr[0]) && isClock(expr[1]));
        break;

    default: ;
    }

    if (invalid)
    {
        handleError(expr, "$Clock_lower_bound_must_be_weak_and_upper_bound_strict");
    }
    else
    {
        switch(expr.getKind()) // No clock differences.
        {
        case LT:
        case LE:
        case GT:
        case GE:
        case EQ:
        case NEQ:
            if  ((isClock(expr[0]) && isClock(expr[1])) ||
                 (isDiff(expr[0]) && isInteger(expr[1])) ||
                 (isInteger(expr[0]) && isDiff(expr[1])))
            {
                handleError(expr, "$Clock_differences_are_not_supported");
            }
            break;

        default: ;
        }
    }
}


static bool validReturnType(type_t type)
{
    frame_t frame;

    switch (type.getKind())
    {
    case Constants::RECORD:
        for (size_t i = 0; i < type.size(); i++)
        {
            if (!validReturnType(type[i]))
            {
                return false;
            }
        }
        return true;

    case Constants::RANGE:
    case Constants::LABEL:
        return validReturnType(type[0]);

    case Constants::INT:
    case Constants::BOOL:
    case Constants::SCALAR:
    case Constants::DOUBLE:
        return true;

    default:
        return false;
    }
}

void TypeChecker::visitFunction(function_t &fun)
{
    SystemVisitor::visitFunction(fun);
    /* Check that the return type is consistent and is a valid return
     * type.
     */
    type_t return_type = fun.uid.getType()[0];
    checkType(return_type);
    if (!return_type.isVoid() && !validReturnType(return_type))
    {
        handleError(return_type, "$Invalid_return_type");
    }

    /* Type check the function body: Type checking return statements
     * requires access to the return type, hence we store a pointer to
     * the current function being type checked in the \a function
     * member.
     */
    function = &fun;
    fun.body->accept(this);
    function = NULL;

    /* Check if there are dynamic expressions in the function body*/
    checkDynamicExpressions (fun.body);

    /* Collect identifiers of things external to the function accessed
     * or changed by the function. Notice that neither local variables
     * nor parameters are considered to be changed or accessed by a
     * function.
     */
    CollectChangesVisitor visitor(fun.changes);
    fun.body->accept(&visitor);

    CollectDependenciesVisitor visitor2(fun.depends);
    fun.body->accept(&visitor2);

    list<variable_t> &vars = fun.variables;
    for (list<variable_t>::iterator i = vars.begin(); i != vars.end(); i++)
    {
        fun.changes.erase(i->uid);
        fun.depends.erase(i->uid);
    }
    size_t parameters = fun.uid.getType().size() - 1;
    for (size_t i = 0; i < parameters; i++)
    {
        fun.changes.erase(fun.body->getFrame()[i]);
        fun.depends.erase(fun.body->getFrame()[i]);
    }
}

int32_t TypeChecker::visitEmptyStatement(EmptyStatement *stat)
{
    return 0;
}

int32_t TypeChecker::visitExprStatement(ExprStatement *stat)
{
    checkAssignmentExpression(stat->expr);
    return 0;
}

int32_t TypeChecker::visitAssertStatement(AssertStatement *stat)
{
    if (checkExpression(stat->expr) && stat->expr.changesAnyVariable())
    {
        handleError(stat->expr, "$Assertion_must_be_side-effect_free");
    }
    return 0;
}

int32_t TypeChecker::visitForStatement(ForStatement *stat)
{
    checkAssignmentExpression(stat->init);

    if (checkExpression(stat->cond))
    {
        checkConditionalExpressionInFunction(stat->cond);
    }

    checkAssignmentExpression(stat->step);

    return stat->stat->accept(this);
}

int32_t TypeChecker::visitIterationStatement(IterationStatement *stat)
{
    type_t type = stat->symbol.getType();
    checkType(type);

    /* We only support iteration over scalars and integers.
     */
    if (!type.isScalar() && !type.isInteger())
    {
        handleError(type, "$Scalar_set_or_integer_expected");
    }
    else if (!type.is(RANGE))
    {
        handleError(type, "$Range_expected");
    }

    return stat->stat->accept(this);
}

int32_t TypeChecker::visitWhileStatement(WhileStatement *stat)
{
    if (checkExpression(stat->cond))
    {
        checkConditionalExpressionInFunction(stat->cond);
    }
    return stat->stat->accept(this);
}

int32_t TypeChecker::visitDoWhileStatement(DoWhileStatement *stat)
{
    if (checkExpression(stat->cond))
    {
        checkConditionalExpressionInFunction(stat->cond);
    }
    return stat->stat->accept(this);
}

int32_t TypeChecker::visitBlockStatement(BlockStatement *stat)
{
    /* Check type and initialiser of local variables (parameters are
     * also considered local variables).
     */
    frame_t frame = stat->getFrame();
    for (uint32_t i = 0; i < frame.getSize(); ++i)
    {
        symbol_t symbol = frame[i];
        checkType(symbol.getType());
        if (symbol.getData())
        {
            variable_t *var = static_cast<variable_t*>(symbol.getData());
            if (!var->expr.empty() && checkExpression(var->expr))
            {
                if (var->expr.changesAnyVariable())
                {
                    /* This is stronger than C. However side-effects in
                     * initialisers are nasty: For records, the evaluation
                     * order may be different from the order in the input
                     * file.
                     */
                    handleError(var->expr, "$Initialiser_must_be_side-effect_free");
                }
                else
                {
                    var->expr = checkInitialiser(symbol.getType(), var->expr);
                }
            }
        }
    }

    /* Check statements.
     */
    for (auto* blockstatement : *stat)
        blockstatement->accept(this);
    return 0;
}

int32_t TypeChecker::visitIfStatement(IfStatement *stat)
{
    if (checkExpression(stat->cond))
    {
        checkConditionalExpressionInFunction(stat->cond);
    }
    stat->trueCase->accept(this);
    if (stat->falseCase)
    {
        stat->falseCase->accept(this);
    }
    return 0;
}

int32_t TypeChecker::visitReturnStatement(ReturnStatement *stat)
{
    if (!stat->value.empty())
    {
        checkExpression(stat->value);

        /* The only valid return types are integers and records. For these
         * two types, the type rules are the same as for parameters.
         */
        type_t return_type = function->uid.getType()[0];
        checkParameterCompatible(return_type, stat->value);
    }
    return 0;
}

/**
 * Returns a value indicating the capabilities of a channel. For
 * urgent channels this is 0, for non-urgent broadcast channels this
 * is 1, and in all other cases 2. An argument to a channel parameter
 * must have at least the same capability as the parameter.
 */
static int channelCapability(type_t type)
{
    assert(type.isChannel());
    if (type.is(URGENT))
    {
        return 0;
    }
    if (type.is(BROADCAST))
    {
        return 1;
    }
    return 2;
}

/**
 * Returns true if two scalar types are name-equivalent.
 */
static bool isSameScalarType(type_t t1, type_t t2)
{
    if (t1.getKind() == REF || t1.getKind() == CONSTANT || t1.getKind() == SYSTEM_META)
    {
        return isSameScalarType(t1[0], t2);
    }
    else if (t2.getKind() == EF || t2.getKind() == CONSTANT || t2.getKind() == SYSTEM_META)
    {
        return isSameScalarType(t1, t2[0]);
    }
    else if (t1.getKind() == LABEL && t2.getKind() == LABEL)
    {
        return t1.getLabel(0) == t2.getLabel(0)
            && isSameScalarType(t1[0], t2[0]);
    }
    else if (t1.getKind() == SCALAR && t2.getKind() == SCALAR)
    {
        return true;
    }
    else if (t1.getKind() == RANGE && t2.getKind() == RANGE)
    {
        return isSameScalarType(t1[0], t2[0])
            && t1.getRange().first.equal(t2.getRange().first)
            && t1.getRange().second.equal(t2.getRange().second);
    }
    else
    {
        return false;
    }
}

/**
 * Returns true iff argument type is compatible with parameter type.
 */
bool TypeChecker::isParameterCompatible(type_t paramType, expression_t arg)
{
    bool ref = paramType.is(REF);
    bool constant = paramType.isConstant();
    bool lvalue = isModifiableLValue(arg);
    type_t argType = arg.getType();
    // For non-const reference parameters, we require a modifiable
    // lvalue argument
    if (ref && !constant && !lvalue)
    {
        return false;
    }

    if (paramType.isChannel() && argType.isChannel())
    {
        return channelCapability(argType) >= channelCapability(paramType);
    }
    else if (ref && lvalue)
    {
        return areEquivalent(argType, paramType);
    }
    else
    {
        return areAssignmentCompatible(paramType, argType);
    }
}

/**
 * Checks whether argument type is compatible with parameter type.
 */
bool TypeChecker::checkParameterCompatible(type_t paramType, expression_t arg)
{
    if (!isParameterCompatible(paramType, arg))
    {
        handleError(arg, "$Incompatible_argument");
        return false;
    }
    return true;
}

/**
 * Checks whether init is a valid initialiser for a variable or
 * constant of the given type. For record types, the initialiser is
 * reordered to fit the order of the fields and the new initialiser is
 * returned. REVISIT: Can a record initialiser have side-effects? Then
 * such reordering is not valid.
 */
expression_t TypeChecker::checkInitialiser(type_t type, expression_t init)
{
    if (areAssignmentCompatible(type, init.getType(), true))
    {
        return init;
    }
    else if (type.isArray() && init.getKind() == LIST)
    {
        type_t subtype = type.getSub();
        vector<expression_t> result(init.getSize(), expression_t());
        for (uint32_t i = 0; i < init.getType().size(); i++)
        {
            if (!init.getType().getLabel(i).empty())
            {
                handleError(
                    init[i], "$Field_name_not_allowed_in_array_initialiser");
            }
            result[i] = checkInitialiser(subtype, init[i]);
        }
        return expression_t::createNary(
            LIST, result, init.getPosition(), type);
    }
    else if (type.isRecord() || init.getKind() == LIST)
    {
        /* In order to access the record labels we have to strip any
         * prefixes and labels from the record type.
         */
        vector<expression_t> result(type.getRecordSize(), expression_t());
        int32_t current = 0;
        for (uint32_t i = 0; i < init.getType().size(); i++, current++)
        {
            std::string label = init.getType().getLabel(i);
            if (!label.empty())
            {
                current = type.findIndexOf(label);
                if (current == -1)
                {
                    handleError(init[i], "$Unknown_field");
                    break;
                }
            }

            if (current >= (int32_t)type.getRecordSize())
            {
                handleError(init[i], "$Too_many_elements_in_initialiser");
                break;
            }

            if (!result[current].empty())
            {
                handleError(init[i], "$Multiple_initialisers_for_field");
                continue;
            }

            result[current] = checkInitialiser(type.getSub(current), init[i]);
        }

        // Check that all fields do have an initialiser.
        for (size_t i = 0; i < result.size(); i++)
        {
            if (result[i].empty())
            {
                handleError(init, "$Incomplete_initialiser");
                break;
            }
        }

        return expression_t::createNary(
            LIST, result, init.getPosition(), type);
    }
    handleError(init, "$Invalid_initialiser");
    return init;
}

/** Returns true if arguments of an inline if are compatible. The
    'then' and 'else' arguments are compatible if and only if they
    have the same base type. In case of arrays, they must have the
    same size and the subtypes must be compatible. In case of records,
    they must have the same type name.
*/
bool TypeChecker::areInlineIfCompatible(type_t t1, type_t t2) const
{
    if (t1.isIntegral() && t2.isIntegral())
    {
        return true;
    }
    else
    {
        return areEquivalent(t1, t2);
    }
}

/**
 * Returns true iff \a a and \a b are structurally
 * equivalent. However, CONST, SYSTEM_META, and REF are ignored. Scalar sets
 * are checked using named equivalence.
 */
bool TypeChecker::areEquivalent(type_t a, type_t b) const
{
    if (a.isInteger() && b.isInteger())
    {
        return !a.is(RANGE)
            || !b.is(RANGE)
            || (a.getRange().first.equal(b.getRange().first)
                && a.getRange().second.equal(b.getRange().second));
    }
    else if (a.isBoolean() && b.isBoolean())
    {
        return true;
    }
    else if (a.isClock() && b.isClock())
    {
        return true;
    }
    else if (a.isChannel() && b.isChannel())
    {
        return channelCapability(a) == channelCapability(b);
    }
    else if (a.isRecord() && b.isRecord())
    {
        size_t aSize = a.getRecordSize();
        size_t bSize = b.getRecordSize();
        if (aSize == bSize)
        {
            for (size_t i = 0; i < aSize; i++)
            {
                if (a.getRecordLabel(i) != b.getRecordLabel(i)
                    || !areEquivalent(a.getSub(i), b.getSub(i)))
                {
                    return false;
                }
            }
            return true;
        }
    }
    else if (a.isArray() && b.isArray())
    {
        type_t asize = a.getArraySize();
        type_t bsize = b.getArraySize();

        if (asize.isInteger() && bsize.isInteger())
        {
            return asize.getRange().first.equal(bsize.getRange().first)
                && asize.getRange().second.equal(bsize.getRange().second)
                && areEquivalent(a.getSub(), b.getSub());
        }
        else if (asize.isScalar() && bsize.isScalar())
        {
            return isSameScalarType(asize, bsize)
                && areEquivalent(a.getSub(), b.getSub());
        }
        return false;
    }
    else if (a.isScalar() && b.isScalar())
    {
        return isSameScalarType(a, b);
    }
    else if (a.isDouble() && b.isDouble())
    {
        return true;
    }

    return false;
}

/** Returns true if lvalue and rvalue are assignment compatible.  This
    is the case when an expression of type rvalue can be assigned to
    an expression of type lvalue. It does not check whether lvalue is
    actually a left-hand side value. In case of integers, it does not
    check the range of the expressions.
*/
bool TypeChecker::areAssignmentCompatible(type_t lvalue, type_t rvalue, bool init) const
{
    if (init
        ? (lvalue.isClock() && rvalue.isDouble())
        : ((lvalue.isClock() || lvalue.isDouble()) &&
           (rvalue.isIntegral() || rvalue.isDouble() || rvalue.isClock())))
    {
        return true;
    }
    else if (lvalue.isIntegral() && rvalue.isIntegral())
    {
        return true;
    }
    return areEquivalent(lvalue, rvalue);
}

/**
 * Returns true if two types are compatible for comparison using the
 * equality operator.
 *
 * Two types are compatible if they are structurally
 * equivalent. However for scalar we use name equivalence.  Prefixes
 * like CONST, SYSTEM_META, URGENT, COMMITTED, BROADCAST, REF and TYPENAME
 * are ignored.
 *
 * Clocks are not handled by this method: If t1 or t2 are clock-types,
 * then false is returned.
 * REVISIT: This should be updated.
 */
bool TypeChecker::areEqCompatible(type_t t1, type_t t2) const
{
    if (t1.isIntegral() && t2.isIntegral())
    {
        return true;
    }
    else if (t1.is(PROCESSVAR) && t2.is(PROCESSVAR))
    {
        return true;
    }
    else
    {
        return areEquivalent(t1, t2);
    }
}

static bool isProcessID(expression_t expr)
{
    return expr.getKind() == IDENTIFIER && expr.getType().is(PROCESS);
}

static bool checkIDList(expression_t expr, kind_t kind)
{
    if (expr.getKind() != LIST)
    {
        return false;
    }
    for(uint32_t j = 0; j < expr.getSize(); ++j)
    {
        if (!isProcessID(expr[j]))
        {
            return false;
        }
    }
    return true;
}

/** Type check and checkExpression the expression. This function performs
    basic type checking of the given expression and assigns a type to
    every subexpression of the expression. It checks that only
    left-hand side values are updated, checks that functions are
    called with the correct arguments, checks that operators are used
    with the correct operands and checks that operands to assignment
    operators are assignment compatible. Errors are reported by
    calling handleError(). This function does not check/compute the
    range of integer expressions and thus does not produce
    out-of-range errors or warnings. Returns true if no type errors
    were found, false otherwise.
*/
bool TypeChecker::checkExpression(expression_t expr)
{
    /* Do not checkExpression empty expressions.
     */
    if (expr.empty())
    {
        return true;
    }

    /* CheckExpression sub-expressions.
     */
    bool ok = true;
    for (uint32_t i = 0; i < expr.getSize(); i++)
    {
        ok &= checkExpression(expr[i]);
    }

    /* Do not checkExpression the expression if any of the sub-expressions
     * contained errors.
     */
    if (!ok)
    {
        return false;
    }

    /* CheckExpression the expression. This depends on the kind of expression
     * we are dealing with.
     */
    type_t type, arg1, arg2, arg3;
    switch (expr.getKind())
    {
        // It is possible to have DOT expressions as data.x
        // with data being an array of struct. The type checker
        // is broken and trying
        // expr[0].getType().isRecord() or
        // expr[0].isProcess() cannot detect this.
        // This should be fixed one day.
        /*
    case DOT:
        if (expr[0].getType().isProcess(Constants::PROCESS) ||
            expr[0].getType().is(Constants::RECORD))
        {
            return true;
        }
        else
        {
            handleError(expr, "$Invalid_type");
            return false;
        }
        */
    case SUMDYNAMIC:
        if (isIntegral(expr[2])  || isDoubleValue (expr[2]))
        {
            type = expr[2].getType ();
        }
        else if (isInvariant (expr[2]) || isGuard (expr[2]))
        {
            type = type_t::createPrimitive(Constants::DOUBLEINVGUARD);
        }
        else {
            handleError (expr,"A sum can only  be made over integer, double, invariant or guard expressions.");
            return false;
        }
        break;
    case FRACTION:
        if (isIntegral(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::FRACTION);
        }
        break;

    case PLUS:
        if (isIntegral(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::INT);
        }
        else if ((isInteger(expr[0]) && isClock(expr[1]))
                 || (isClock(expr[0]) && isInteger(expr[1])))
        {
            type = type_t::createPrimitive(CLOCK);
        }
        else if ((isDiff(expr[0]) && isInteger(expr[1]))
                 || (isInteger(expr[0]) && isDiff(expr[1])))
        {
            type = type_t::createPrimitive(DIFF);
        }
        else if (isNumber(expr[0]) && isNumber(expr[1]))
        {
            // SMC extension.
            type = type_t::createPrimitive(Constants::DOUBLE);
        }
        break;

    case MINUS:
        if (isIntegral(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::INT);
        }
        else if (isClock(expr[0]) && isInteger(expr[1]))
            // removed  "|| isInteger(expr[0].type) && isClock(expr[1].type)"
            // in order to be able to convert into ClockGuards
        {
            type = type_t::createPrimitive(CLOCK);
        }
        else if ((isDiff(expr[0]) && isInteger(expr[1]))
                 || (isInteger(expr[0]) && isDiff(expr[1]))
                 || (isClock(expr[0]) && isClock(expr[1])))
        {
            type = type_t::createPrimitive(DIFF);
        }
        else if (isNumber(expr[0]) && isNumber(expr[1]))
        {
            // SMC extension.
            // x-y with that semantic should be written x+(-y)
            type = type_t::createPrimitive(Constants::DOUBLE);
        }
        break;

    case AND:
        if (isIntegral(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        else if (isInvariant(expr[0]) && isInvariant(expr[1]))
        {
            type = type_t::createPrimitive(INVARIANT);
        }
        else if (isInvariantWR(expr[0]) && isInvariantWR(expr[1]))
        {
            type = type_t::createPrimitive(INVARIANT_WR);
        }
        else if (isGuard(expr[0]) && isGuard(expr[1]))
        {
            type = type_t::createPrimitive(GUARD);
        }
        else if (isConstraint(expr[0]) && isConstraint(expr[1]))
        {
            type = type_t::createPrimitive(CONSTRAINT);
        }
        else if (isFormula(expr[0]) && isFormula(expr[1]))
        {
            type = type_t::createPrimitive(FORMULA);
        }
        break;

    case OR:
        if (isIntegral(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        else if (isIntegral(expr[0]) && isInvariant(expr[1]))
        {
            type = type_t::createPrimitive(INVARIANT);
        }
        else if (isInvariant(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(INVARIANT);
        }
        else if (isIntegral(expr[0]) && isInvariantWR(expr[1]))
        {
            type = type_t::createPrimitive(INVARIANT_WR);
        }
        else if (isInvariantWR(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(INVARIANT_WR);
        }
        else if (isIntegral(expr[0]) && isGuard(expr[1]))
        {
            type = type_t::createPrimitive(GUARD);
        }
        else if (isGuard(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(GUARD);
        }
        else if (isConstraint(expr[0]) && isConstraint(expr[1]))
        {
            type = type_t::createPrimitive(CONSTRAINT);
        }
        break;

    case XOR:
        if (isIntegral(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        break;

    case SPAWN: {
        template_t* temp = system->getDynamicTemplate(expr[0].getSymbol().getName());
        if (!temp) {
            handleError(expr, "Appears as an attempt to spawn a non-dynamic template");
            return false;
        }
        if (temp->parameters.getSize() != expr.getSize()-1) {
            handleError(expr, "Wrong number of arguments");
            return false;
        }
        for (size_t i = 0; i<temp->parameters.getSize(); i++) {
            if (!checkSpawnParameterCompatible(temp->parameters[i].getType(),
                                               expr[i+1]))
            {
                return false;
            }
        }

        /* check that spawn is only made for templates with definitions*/
        if (!temp->isDefined) {
            handleError (expr,"Template is only declared - not defined");
            return false;
        }
        type = type_t::createPrimitive(Constants::INT);
        break;
    }

    case NUMOF: {
        template_t* temp = system->getDynamicTemplate (expr[0].getSymbol ().getName ());
        if (temp) {
            type = type_t::createPrimitive (Constants::INT);
        }
        else
        {
            handleError (expr,"Not a dynamic template");
            return false;
        }
        break;
    }

    case EXIT:
    {
        assert(temp);
        if (!temp->dynamic) {
            handleError (expr,"Exit can only be used in templates declared as dynamic");
            return false;
        }

        type = type_t::createPrimitive (Constants::INT);

        break;
    }

    case EQ:
        // FIXME: Apparently the case cost == expr goes through, which is obviously not good.

        if ((isClock(expr[0]) && isClock(expr[1]))
            || (isClock(expr[0]) && isInteger(expr[1]))
            || (isInteger(expr[0]) && isClock(expr[1]))
            || (isDiff(expr[0]) && isInteger(expr[1]))
            || (isInteger(expr[0]) && isDiff(expr[1])))
        {
            type = type_t::createPrimitive(GUARD);
        }
        else if (areEqCompatible(expr[0].getType(), expr[1].getType()))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        else if ((expr[0].getType().is(RATE) &&
                  (isIntegral(expr[1]) || isDoubleValue(expr[1]))) ||
                 ((isIntegral(expr[0]) || isDoubleValue(expr[0])) &&
                  expr[1].getType().is(RATE)))
        {
            type = type_t::createPrimitive(INVARIANT_WR);
        }
        else if (isNumber(expr[0]) && isNumber(expr[1]))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        break;

    case NEQ:
        if (areEqCompatible(expr[0].getType(), expr[1].getType()))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        else if ((isClock(expr[0]) && isClock(expr[1]))
                 || (isClock(expr[0]) && isInteger(expr[1]))
                 || (isInteger(expr[0]) && isClock(expr[1]))
                 || (isDiff(expr[0]) && isInteger(expr[1]))
                 || (isInteger(expr[0]) && isDiff(expr[1])))
        {
            type = type_t::createPrimitive(CONSTRAINT);
        }
        else if (isNumber(expr[0]) && isNumber(expr[1]))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        break;

    case GE:
    case GT:
    case LE:
    case LT:
        if (isIntegral(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        else if ((isClock(expr[0]) && isClock(expr[1])) ||
                 (isClock(expr[0]) && isBound(expr[1])) ||
                 (isClock(expr[1]) && isBound(expr[0])) ||
                 (isDiff(expr[0]) && isBound(expr[1])) ||
                 (isDiff(expr[1]) && isBound(expr[0])))
        {
            type = type_t::createPrimitive(INVARIANT);
        }
        else if ((isClock(expr[0]) && isInteger(expr[1])) ||
                 (isInteger(expr[0]) && isClock(expr[1])))
        {
            type = type_t::createPrimitive(GUARD);
        }
        else if (isNumber(expr[0]) && isNumber(expr[1]))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        break;

    case MULT:
    case DIV:
    case MIN:
    case MAX:
        if (isIntegral(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::INT);
        }
        else if (isNumber(expr[0]) && isNumber(expr[1]))
        {
            type = type_t::createPrimitive(Constants::DOUBLE);
        }
        break;

    case MOD:
    case BIT_AND:
    case BIT_OR:
    case BIT_XOR:
    case BIT_LSHIFT:
    case BIT_RSHIFT:
        if (isIntegral(expr[0]) && isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::INT);
        }
        break;

    case NOT:
        if (isIntegral(expr[0]))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        else if (isConstraint(expr[0]))
        {
            type = type_t::createPrimitive(CONSTRAINT);
        }
        break;

    case UNARY_MINUS:
        if (isIntegral(expr[0]))
        {
            type = type_t::createPrimitive(Constants::INT);
        }
        else if (isNumber(expr[0]))
        {
            type = type_t::createPrimitive(Constants::DOUBLE);
        }
        break;

    case RATE:
        if (isCost(expr[0]) || isClock(expr[0]))
        {
            type = type_t::createPrimitive(RATE);
        }
        break;

    case ASSIGN:
        if (!areAssignmentCompatible(expr[0].getType(), expr[1].getType()))
        {
            handleError(expr, "$Incompatible_types");
            return false;
        }
        else if (!isModifiableLValue(expr[0]))
        {
            handleError(expr[0], "$Left_hand_side_value_expected");
            return false;
        }
        type = expr[0].getType();
        break;

    case ASSPLUS:
        if ((!isInteger(expr[0]) && !isCost(expr[0])) || !isIntegral(expr[1]))
        {
            handleError(expr, "$Increment_operator_can_only_be_used_for_integers_and_cost_variables");
        }
        else if (!isModifiableLValue(expr[0]))
        {
            handleError(expr[0], "$Left_hand_side_value_expected");
        }
        type = expr[0].getType();
        break;

    case ASSMINUS:
    case ASSDIV:
    case ASSMOD:
    case ASSMULT:
    case ASSAND:
    case ASSOR:
    case ASSXOR:
    case ASSLSHIFT:
    case ASSRSHIFT:
        if (!isIntegral(expr[0]) || !isIntegral(expr[1]))
        {
            handleError(expr, "$Non-integer_types_must_use_regular_assignment_operator");
            return false;
        }
        else if (!isModifiableLValue(expr[0]))
        {
            handleError(expr[0], "$Left_hand_side_value_expected");
            return false;
        }
        type = expr[0].getType();
        break;

    case POSTINCREMENT:
    case PREINCREMENT:
    case POSTDECREMENT:
    case PREDECREMENT:
        if (!isModifiableLValue(expr[0]))
        {
            handleError(expr[0], "$Left_hand_side_value_expected");
            return false;
        }
        else if (!isInteger(expr[0]))
        {
            handleError(expr, "$Integer_expected");
            return false;
        }
        type = type_t::createPrimitive(Constants::INT);
        break;

    case FMA_F:
    case RANDOM_TRI_F:
        if (!isNumber(expr[2]))
        {
            handleError(expr[2], "$Number_expected");
            return false;
        } // fall-through to check the second argument:
    case FMOD_F:
    case FMAX_F:
    case FMIN_F:
    case FDIM_F:
    case POW_F:
    case HYPOT_F:
    case ATAN2_F:
    case NEXTAFTER_F:
    case COPYSIGN_F:
    case RANDOM_ARCSINE_F:
    case RANDOM_BETA_F:
    case RANDOM_GAMMA_F:
    case RANDOM_NORMAL_F:
    case RANDOM_WEIBULL_F:
        if (!isNumber(expr[1]))
        {
            handleError(expr[1], "$Number_expected");
            return false;
        } // fall-through to check the first argument:
    case FABS_F:
    case EXP_F:
    case EXP2_F:
    case EXPM1_F:
    case LN_F:
    case LOG_F:
    case LOG10_F:
    case LOG2_F:
    case LOG1P_F:
    case SQRT_F:
    case CBRT_F:
    case SIN_F:
    case COS_F:
    case TAN_F:
    case ASIN_F:
    case ACOS_F:
    case ATAN_F:
    case SINH_F:
    case COSH_F:
    case TANH_F:
    case ASINH_F:
    case ACOSH_F:
    case ATANH_F:
    case ERF_F:
    case ERFC_F:
    case TGAMMA_F:
    case LGAMMA_F:
    case CEIL_F:
    case FLOOR_F:
    case TRUNC_F:
    case ROUND_F:
    case LOGB_F:
    case RANDOM_F:
    case RANDOM_POISSON_F:
        if (!isNumber(expr[0]))
        {
            handleError(expr[0], "$Number_expected");
            return false;
        }
        type = type_t::createPrimitive(Constants::DOUBLE);
        break;

    case LDEXP_F:
        if (!isIntegral(expr[1]))
        {
            handleError(expr[1], "$Integer_expected");
            return false;
        }
        if (!isNumber(expr[0]))
        {
            handleError(expr[0], "$Number_expected");
            return false;
        }
        type = type_t::createPrimitive(Constants::DOUBLE);
        break;

    case ABS_F:
    case FPCLASSIFY_F:
        if (!isIntegral(expr[0]))
        {
            handleError(expr[0], "$Integer_expected");
            return false;
        }
        type = type_t::createPrimitive(Constants::INT);
        break;

    case ILOGB_F:
    case FINT_F:
        if (!isNumber(expr[0]))
        {
            handleError(expr[0], "$Number_expected");
            return false;
        }
        type = type_t::createPrimitive(Constants::INT);
        break;

    case ISFINITE_F:
    case ISINF_F:
    case ISNAN_F:
    case ISNORMAL_F:
    case SIGNBIT_F:
    case ISUNORDERED_F:
        if (!isNumber(expr[0]))
        {
            handleError(expr[0], "$Number_expected");
            return false;
        }
        type = type_t::createPrimitive(Constants::BOOL);
        break;

    case INLINEIF:
        if (!isIntegral(expr[0]))
        {
            handleError(expr, "$First_argument_of_inline_if_must_be_an_integer");
            return false;
        }
        if (!areInlineIfCompatible(expr[1].getType(), expr[2].getType()))
        {
            handleError(expr, "$Incompatible_arguments_to_inline_if");
            return false;
        }
        type = expr[1].getType();
        break;

    case COMMA:
        if (!isAssignable(expr[0].getType()) && !isVoid(expr[0]))
        {
            handleError(expr[0], "$Incompatible_type_for_comma_expression");
            return false;
        }
        if (!isAssignable(expr[1].getType()) && !isVoid(expr[1]))
        {
            handleError(expr[1], "$Incompatible_type_for_comma_expression");
            return false;
        }
        checkIgnoredValue(expr[0]);
        type = expr[1].getType();
        break;

    case FUNCALL:
    {
        checkExpression(expr[0]);

        bool result = true;
        type_t type = expr[0].getType();
        size_t parameters = type.size() - 1;
        for (uint32_t i = 0; i < parameters; i++)
        {
            type_t parameter = type[i + 1];
            expression_t argument = expr[i + 1];
            result &= checkParameterCompatible(parameter, argument);
        }
        return result;
    }

    case ARRAY:
        arg1 = expr[0].getType();
        arg2 = expr[1].getType();

        /* The left side must be an array.
         */
        if (!arg1.isArray())
        {
            handleError(expr[0], "$Array_expected");
            return false;
        }
        type = arg1.getSub();

        /* Check the type of the index.
         */
        if (arg1.getArraySize().isInteger() && arg2.isIntegral())
        {

        }
        else if (arg1.getArraySize().isScalar() && arg2.isScalar())
        {
            if (!isSameScalarType(arg1.getArraySize(), arg2))
            {
                handleError(expr[1], "$Incompatible_type");
                return false;
            }
        }
        else
        {
            handleError(expr[1], "$Incompatible_type");
        }
        break;


    case FORALL:
        checkType(expr[0].getSymbol().getType());

        if (isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        else if (isInvariant(expr[1]))
        {
            type = type_t::createPrimitive(INVARIANT);
        }
        else if (isInvariantWR(expr[1]))
        {
            type = type_t::createPrimitive(INVARIANT_WR);
        }
        else if (isGuard(expr[1]))
        {
            type = type_t::createPrimitive(GUARD);
        }
        else if (isConstraint(expr[1]))
        {
            type = type_t::createPrimitive(CONSTRAINT);
        }
        else
        {
            handleError(expr[1], "$Boolean_expected");
        }

        if (expr[1].changesAnyVariable())
        {
            handleError(expr[1], "$Expression_must_be_side-effect_free");
        }
        break;

    case EXISTS:
        checkType(expr[0].getSymbol().getType());

        if (isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::BOOL);
        }
        else if (isConstraint(expr[1]))
        {
            type = type_t::createPrimitive(CONSTRAINT);
        }
        else
        {
            handleError(expr[1], "$Boolean_expected");
        }

        if (expr[1].changesAnyVariable())
        {
            handleError(expr[1], "$Expression_must_be_side-effect_free");
        }
        break;

    case SUM:
        checkType(expr[0].getSymbol().getType());

        if (isIntegral(expr[1]))
        {
            type = type_t::createPrimitive(Constants::INT);
        }
        else if (isNumber(expr[1]))
        {
            type = type_t::createPrimitive(Constants::DOUBLE);
        }
        else
        {
            handleError(expr[1], "$Number_expected");
        }

        if (expr[1].changesAnyVariable())
        {
            handleError(expr[1], "$Expression_must_be_side-effect_free");
        }
        break;

    case AF:
    case AG:
    case EF:
    case EG:
    case EF_R_Piotr:
    case AG_R_Piotr:
    case EF_CONTROL:
    case CONTROL:
    case CONTROL_TOPT:
    case CONTROL_TOPT_DEF1:
    case CONTROL_TOPT_DEF2:
    case PMAX:
        if (isFormula(expr[0]))
        {
            type = type_t::createPrimitive(FORMULA);
        }
        break;

    case PO_CONTROL:
        if (isListOfFormulas(expr[0]) && isFormula(expr[1]))
        {
            type = type_t::createPrimitive(FORMULA);
        }
        break;

    case RESTRICT:
        if (!checkIDList(expr[0], PROCESS))
        {
            handleError(expr[0], "$Composition_of_processes_expected");
            ok = false;
        }
        if (!checkIDList(expr[1], CHANNEL))
        {
            handleError(expr[1], "$List_of_channels_expected");
            ok = false;
        }
        if (!ok)
        {
            return false;
        }
        type = type_t::createPrimitive(FORMULA);
        break;

    case SIMULATION_LE:
    case SIMULATION_GE:
    {
        bool le = expr.getKind() == SIMULATION_LE;
        expression_t &e1 = le ? expr[0] : expr[1];
        expression_t &e2 = le ? expr[1] : expr[0];
        if (e1.getKind() != RESTRICT)
        {
            handleError(e1, "$Composition_of_processes_expected");
            ok = false;
        }
        if (!checkIDList(e2, PROCESS))
        {
            handleError(e2, "$Composition_of_processes_expected");
            ok = false;
        }
        if (!ok)
        {
            return false;
        }
        type = type_t::createPrimitive(FORMULA);
        break;
    }

    case TIOQUOTIENT: /* (Graph | Id) + (Graph | Id) */
        if (!expr[0].getType().is(TIOGRAPH) && !isProcessID(expr[0]))
        {
            handleError(expr[0], "$Process_expression_expected");
            ok = false;
        }
        if (!expr[1].getType().is(TIOGRAPH) && !isProcessID(expr[1]))
        {
            handleError(expr[1], "$Process_expression_expected");
            ok = false;
        }
        if (!ok)
        {
            return false;
        }
        type = type_t::createPrimitive(TIOGRAPH);
        break;

    case CONSISTENCY: /* (Graph | Id) + Expr */
        if (!expr[0].getType().is(TIOGRAPH) && !isProcessID(expr[0]))
        {
            handleError(expr[0], "$Process_expression_expected");
            ok = false;
        }
        if (!isFormula(expr[1]))
        {
            handleError(expr[1], "$Property_must_be_a_valid_formula");
            ok = false;
        }
        if (!ok)
        {
            return false;
        }
        type = type_t::createPrimitive(TIOGRAPH);
        break;

    case SPECIFICATION:
    case IMPLEMENTATION:
        if (!expr[0].getType().is(TIOGRAPH) && !isProcessID(expr[0]))
        {
            handleError(expr[0], "$Process_expression_expected");
            return false;
        }
        type = type_t::createPrimitive(FORMULA);
        break;

    case TIOCOMPOSITION:
    case TIOCONJUNCTION:
    case SYNTAX_COMPOSITION:
        for(uint32_t i = 0; i < expr.getSize(); ++i)
        {
            if (!expr[i].getType().is(TIOGRAPH) && expr[i].getKind() != IDENTIFIER)
            {
                handleError(expr[i], "$Process_expression_expected");
                ok = false;
            }
        }
        if (!ok)
        {
            return false;
        }
        type = type_t::createPrimitive(TIOGRAPH);
        break;

    case REFINEMENT_LE:
    case REFINEMENT_GE:
        if (!expr[0].getType().is(TIOGRAPH) && expr[0].getKind() != IDENTIFIER)
        {
            handleError(expr[0], "$Process_expression_expected");
            ok = false;
        }
        if (!expr[1].getType().is(TIOGRAPH) && expr[1].getKind() != IDENTIFIER)
        {
            handleError(expr[0], "$Process_expression_expected");
            ok = false;
        }
        if (!ok)
        {
            return false;
        }
        type = type_t::createPrimitive(FORMULA);
        break;

    case LEADSTO:
    case SCENARIO2:
    case A_UNTIL:
    case A_WEAKUNTIL:
    case A_BUCHI:
        if (isFormula(expr[0]) && isFormula(expr[1]))
        {
            type = type_t::createPrimitive(FORMULA);
        }
        break;

    case SCENARIO:
        type = type_t::createPrimitive(FORMULA);
        break;

    case SIMULATEREACH:
    case SIMULATE:
    {
        bool ok = true;
        ok &= checkNrOfRuns(expr[0]);
        if (ok && expr[0].getValue() <= 0)
        {
            handleError(expr[0], "$Invalid_run_count");
            ok = false;
        }
        ok &= checkBoundTypeOrBoundedExpr(expr[1]);
        ok &= checkBound(expr[2]);
        if (!ok)
            return false;
        int nb = expr.getSize();
        if (expr.getKind() == SIMULATEREACH)
        {
            bool ok = true;
            nb -= 2;
            ok &= checkPredicate(expr[nb]);
            ok &= checkNrOfRuns(expr[nb+1]);
            if (!ok)
                return false;
        }
        for (int i = 3; i < nb; ++i)
            if (!checkMonitoredExpr(expr[i]))
                return false;

        type = type_t::createPrimitive(FORMULA);
        break;
    }

    case SUP_VAR:
    case INF_VAR:
        if (!isIntegral(expr[0]) && !isConstraint(expr[0]))
        {
            handleError(expr[0], "$Boolean_expected");
            return false;
        }
        if (expr[1].getKind() == LIST)
        {
            for(uint32_t i = 0; i < expr[1].getSize(); ++i)
            {
                if (isIntegral(expr[1][i]))
                {
                    if (expr[1][i].changesAnyVariable())
                    {
                        handleError(expr[1][i], "$Expression_must_be_side-effect_free");
                        return false;
                    }
                }
                else if (!isClock(expr[1][i]))
                {
                    handleError(expr[1][i], "$Type_error");
                    return false;
                }
            }
            type = type_t::createPrimitive(FORMULA);
        }
        break;

    case MITLFORMULA:
    case MITLCONJ:
    case MITLDISJ:
    case MITLNEXT:
    case MITLUNTIL:
    case MITLRELEASE:
    case MITLATOM:
         type = type_t::createPrimitive(FORMULA);
        //TODO
        break;

    case SMC_CONTROL:
        assert(expr.getSize() == 3);
        ok &= checkBoundTypeOrBoundedExpr(expr[0]);
        ok &= checkBound(expr[1]);
        if (!ok)
            return false;
        if (isFormula(expr[2]))
        {
            type = type_t::createPrimitive(FORMULA);
        }
        break;

    case PROBAMINBOX:
    case PROBAMINDIAMOND:
        if (expr.getSize() == 5)
        {
            bool ok = true;
            ok &= checkNrOfRuns(expr[0]);
            if (ok && expr[0].getValue() > 0)
            {
                handleError(expr[0],"Explicit number of runs is not supported here");
                ok = false;
            }
            ok &= checkBoundTypeOrBoundedExpr(expr[1]);
            ok &= checkBound(expr[2]);
            ok &= checkPredicate(expr[3]);
            ok &= checkProbBound(expr[4]);
            if (!ok)
                return false;
            type = type_t::createPrimitive(FORMULA);
        } else {
            handleError(expr, "Bug: wrong number of arguments");
            return false;
        }
        break;

    case PROBABOX:
    case PROBADIAMOND:
        if (expr.getSize() == 5)
        {
            bool ok = true;
            ok &= checkNrOfRuns(expr[0]);
            ok &= checkBoundTypeOrBoundedExpr(expr[1]);
            ok &= checkBound(expr[2]);
            ok &= checkPredicate(expr[3]);
            ok &= checkUntilCond(expr.getKind(), expr[4]);
            if (!ok)
                return false;
            type = type_t::createPrimitive(FORMULA);
        } else {
            handleError(expr, "Bug: wrong number of arguments");
            return false;
        }
        break;

    case PROBACMP:
        if (expr.getSize() == 8)
        {
            bool ok = true;
            // the first prob property:
            ok &= checkBoundTypeOrBoundedExpr(expr[0]);
            ok &= checkBound(expr[1]);
            ok &= checkPathQuant(expr[2]);
            ok &= checkPredicate(expr[3]);
            // the second prob property:
            ok &= checkBoundTypeOrBoundedExpr(expr[4]);
            ok &= checkBound(expr[5]);
            ok &= checkPathQuant(expr[6]);
            ok &= checkPredicate(expr[7]);
            if (!ok)
                return false;
            type = type_t::createPrimitive(FORMULA);
        } else {
            handleError(expr, "Bug: wrong number of arguments");
            return false;
        }
        break;

    case PROBAEXP:
        if (expr.getSize() == 5)
        {   // Encoded by expressionbuilder.
            bool ok = true;
            ok &= checkNrOfRuns(expr[0]);
            ok &= checkBoundTypeOrBoundedExpr(expr[1]);
            ok &= checkBound(expr[2]);
            ok &= checkAggregationOp(expr[3]);
            ok &= checkMonitoredExpr(expr[4]);
            if (!ok)
                return false;
            type = type_t::createPrimitive(FORMULA);
        } else {
            handleError(expr, "Bug: wrong number of arguments");
            return false;
        }
        break;

    default:
        return true;
    }

    if (type.unknown())
    {
        handleError(expr, "$Type_error");
        return false;
    }
    else
    {
        expr.setType(type);
        return true;
    }
}

/**
 * Returns true if expression evaluates to a modifiable l-value.
 */
bool TypeChecker::isModifiableLValue(expression_t expr) const
{
    type_t t, f;
    switch (expr.getKind())
    {
    case IDENTIFIER:
        return expr.getType().isNonConstant();

    case DOT:
        /* Processes can only be used in properties, which must be
         * side-effect anyway. Therefore returning false below is
         * acceptable for now (REVISIT).
         */
        if (expr[0].getType().isProcess())
        {
            return false;
        }
        // REVISIT: Not correct if records contain constant fields.
        return isModifiableLValue(expr[0]);

    case ARRAY:
        return isModifiableLValue(expr[0]);

    case PREINCREMENT:
    case PREDECREMENT:
    case ASSIGN:
    case ASSPLUS:
    case ASSMINUS:
    case ASSDIV:
    case ASSMOD:
    case ASSMULT:
    case ASSAND:
    case ASSOR:
    case ASSXOR:
    case ASSLSHIFT:
    case ASSRSHIFT:
        return true;

    case INLINEIF:
        return isModifiableLValue(expr[1]) && isModifiableLValue(expr[2])
            && areEquivalent(expr[1].getType(), expr[2].getType());

    case COMMA:
        return isModifiableLValue(expr[1]);

    case FUNCALL:
        // Functions cannot return references (yet!)

    default:
        return false;
    }
}

/**
 * Returns true iff \a expr evaluates to an lvalue.
 */
bool TypeChecker::isLValue(expression_t expr) const
{
    type_t t, f;
    switch (expr.getKind())
    {
    case IDENTIFIER:
    case PREINCREMENT:
    case PREDECREMENT:
    case ASSIGN:
    case ASSPLUS:
    case ASSMINUS:
    case ASSDIV:
    case ASSMOD:
    case ASSMULT:
    case ASSAND:
    case ASSOR:
    case ASSXOR:
    case ASSLSHIFT:
    case ASSRSHIFT:
        return true;

    case DOT:
    case ARRAY:
        return isLValue(expr[0]);

    case INLINEIF:
        return isLValue(expr[1]) && isLValue(expr[2])
            && areEquivalent(expr[1].getType(), expr[2].getType());

    case COMMA:
        return isLValue(expr[1]);

    case FUNCALL:
        // Functions cannot return references (yet!)

    default:
        return false;
    }
}

/** Returns true if expression is a reference to a unique variable.
    This is similar to expr being an l-value, but in addition we
    require that the reference does not depend on any non-computable
    expressions. Thus i[v] is a l-value, but if v is a non-constant
    variable, then it does not result in a unique reference.
*/
bool TypeChecker::isUniqueReference(expression_t expr) const
{
    switch (expr.getKind())
    {
    case IDENTIFIER:
        return true;

    case DOT:
        return isUniqueReference(expr[0]);

    case ARRAY:
        return isUniqueReference(expr[0])
            && isCompileTimeComputable(expr[1]);

    case PREINCREMENT:
    case PREDECREMENT:
    case ASSIGN:
    case ASSPLUS:
    case ASSMINUS:
    case ASSDIV:
    case ASSMOD:
    case ASSMULT:
    case ASSAND:
    case ASSOR:
    case ASSXOR:
    case ASSLSHIFT:
    case ASSRSHIFT:
        return isUniqueReference(expr[0]);

    case INLINEIF:
        return false;

    case COMMA:
        return isUniqueReference(expr[1]);

    case FUNCALL:
        // Functions cannot return references (yet!)

    default:
        return false;
    }
}

bool parseXTA(FILE *file, TimedAutomataSystem *system, bool newxta)
{
    SystemBuilder builder(system);
    parseXTA(file, &builder, newxta);
    if (!system->hasErrors())
    {
        TypeChecker checker(system);
        system->accept(checker);
    }
    return !system->hasErrors();
}

bool parseXTA(const char *buffer, TimedAutomataSystem *system, bool newxta)
{
    SystemBuilder builder(system);
    parseXTA(buffer, &builder, newxta);
    if (!system->hasErrors())
    {
        TypeChecker checker(system);
        system->accept(checker);
    }
    return !system->hasErrors();
}

int32_t parseXMLBuffer(const char *buffer, TimedAutomataSystem *system, bool newxta)
{
    int err;

    SystemBuilder builder(system);
    err = parseXMLBuffer(buffer, &builder, newxta);

    if (err)
    {
        return err;
    }

    if (!system->hasErrors())
    {
        TypeChecker checker(system);
        system->accept(checker);
    }

    return 0;
}

int32_t parseXMLFile(const char *file, TimedAutomataSystem *system, bool newxta)
{
    int err;

    SystemBuilder builder(system);
    err = parseXMLFile(file, &builder, newxta);
    if (err)
    {
        return err;
    }

    if (!system->hasErrors())
    {
        TypeChecker checker(system);
        system->accept(checker);
    }

    return 0;
}

expression_t parseExpression(const char *str,
                             TimedAutomataSystem *system, bool newxtr)
{
    ExpressionBuilder builder(system);
    parseXTA(str, &builder, newxtr, S_EXPRESSION, "");
    expression_t expr = builder.getExpressions()[0];
    if (!system->hasErrors())
    {
        TypeChecker checker(system);
        checker.checkExpression(expr);
    }
    return expr;
}

void TypeChecker::visitTemplateAfter (template_t& t) {
    assert(&t == temp);

    temp = NULL;
}

bool TypeChecker::visitTemplateBefore (template_t& t) {
    assert(!temp);
    temp = &t;

    return true;

}

bool TypeChecker::checkSpawnParameterCompatible(type_t param, expression_t arg)
{
    return checkParameterCompatible(param,arg);
}

bool TypeChecker::checkDynamicExpressions(Statement* stat)
{
    std::list<expression_t> l;
    CollectDynamicExpressions e(l);
    stat->accept(&e);
    bool ok = true;
    for (auto& it: l)
    {
        ok = false;
        handleError(it, "Dynamic constructs are only allowed on edges!");
    }
    return ok;
}

bool TypeChecker::checkNrOfRuns(const expression_t& runs)
{
    if (!isCompileTimeComputable(runs))
    {
        handleError(runs, "$Must_be_computable_at_compile_time");
        return false;
    }
    if (!isConstantInteger(runs))
    {
        handleError(runs, "$Integer_expected");
        return false;
    }
    return true;
}

bool TypeChecker::checkBoundTypeOrBoundedExpr(const expression_t& boundTypeOrExpr)
{
    if (!isConstantInteger(boundTypeOrExpr) && !isClock(boundTypeOrExpr))
    {
        handleError(boundTypeOrExpr, "$Clock_expected");
        return false;
    }
    return true;
}

bool TypeChecker::checkBound(const expression_t& bound)
{
    if (!isCompileTimeComputable(bound))
    {
        handleError(bound, "$Must_be_computable_at_compile_time");
        return false;
    }
    if (!isIntegral(bound))
    {
        handleError(bound, "$Integer_expected");
        return false;
    }
    return true;
}

bool TypeChecker::checkPredicate(const expression_t& predicate)
{
    if (!isIntegral(predicate) && !isConstraint(predicate))
    {   //check reachability expression is a boolean
        handleError(predicate, "$Boolean_expected");
        return false;
    }
    if (predicate.changesAnyVariable())
    {   //check reachability expression is side effect free
        handleError(predicate, "$Property_must_be_side-effect_free");
        return false;
    }
    return true;
}

bool TypeChecker::checkProbBound(const expression_t& probBound)
{
    if (!isConstantDouble(probBound))
    {
        handleError(probBound, "Floating point number expected as probability bound");
        return false;
    }
    return true;
}

bool TypeChecker::checkUntilCond(kind_t kind, const expression_t& untilCond)
{
    bool ok = true;
    if (kind == PROBADIAMOND && !isIntegral(untilCond) && !isConstraint(untilCond))
    {
        handleError(untilCond, "$Boolean_expected");
        ok = false;
    }
    if (kind == PROBABOX && untilCond.getKind()==Constants::BOOL &&
        untilCond.getValue() != 0)
    {
        handleError(untilCond, "Must be false"); //TODO - error message
        ok = false;
    }
    return ok;
}

bool TypeChecker::checkMonitoredExpr(const expression_t& expr)
{
    if (!isIntegral(expr) && !isClock(expr) && !isDoubleValue(expr) &&
        !expr.getType().is(Constants::DOUBLEINVGUARD) &&
        !isConstraint(expr))
    {
        handleError(expr, "$Integer_or_clock_expected");
        return false;
    }
    if (expr.changesAnyVariable())
    {
        handleError(expr, "$Property_must_be_side-effect_free");
        return false;
    }
    return true;
}

bool TypeChecker::checkPathQuant(const expression_t& expr)
{
    if (!isConstantInteger(expr))
    {
        handleError(expr, "Bug: bad path quantifier");
        return false;
    }
    return true;
}

bool TypeChecker::checkAggregationOp(const expression_t& expr)
{
    if (!isConstantInteger(expr))
    {
        handleError(expr, "Bug: bad aggregation operator expression");
        return false;
    }
    if (expr.getValue()>1) // 0="min", 1="max"
    {
        handleError(expr, "Bug: bad aggregation operator value");
        return false;
    }
    return true;
}
