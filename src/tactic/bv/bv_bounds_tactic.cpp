/*++
Copyright (c) 2016 Microsoft Corporation

Module Name:

    bv_bounds_tactic.cpp

Abstract:

    Contextual bounds simplification tactic.

Author:

    Nikolaj Bjorner (nbjorner) 2016-2-12


--*/

#include "bv_bounds_tactic.h"
#include "ctx_simplify_tactic.h"
#include "bv_decl_plugin.h"
#include "ast_pp.h"

static rational uMaxInt(unsigned sz) {
    return rational::power_of_two(sz) - rational::one();
}

namespace {

struct interval {
    // l < h: [l, h]
    // l > h: [0, h] U [l, UMAX_INT]
    rational l, h;
    unsigned sz;

    explicit interval() : l(0), h(0), sz(0) {}
    interval(const rational& l, const rational& h, unsigned sz) : l(l), h(h), sz(sz) {
        SASSERT(invariant());
    }

    bool invariant() const {
        return !l.is_neg() && !h.is_neg() && l <= uMaxInt(sz) && h <= uMaxInt(sz);
    }

    bool is_full() const { return l.is_zero() && h == uMaxInt(sz); }
    bool is_wrapped() const { return l > h; }

    bool implies(const interval& b) const {
        if (b.is_full())
            return true;
        if (is_full())
            return false;

        if (is_wrapped()) {
            // l >= b.l >= b.h >= h
            return b.is_wrapped() && h <= b.h && l >= b.l;
        } else if (b.is_wrapped()) {
            // b.l > b.h >= h >= l
            // h >= l >= b.l > b.h
            return h <= b.h || l >= b.l;
        } else {
            // 
            return l >= b.l && h <= b.h;
        }
    }

    /// return false if intersection is unsat
    bool intersect(const interval& b, interval& result) const {
        if (is_full() || (l == b.l && h == b.h)) {
            result = b;
            return true;
        }
        if (b.is_full()) {
            result = *this;
            return true;
        }

        if (is_wrapped()) {
            if (b.is_wrapped()) {
                if (h > b.l) {
                    result = b;
                } else if (b.h > l) {
                    result = *this;
                } else {
                    result = interval(std::max(l, b.l), std::min(h, b.h), sz);
                }
            } else {
                return b.intersect(*this, result);
            }
        } else if (b.is_wrapped()) {
            // ... b.h ... l ... h ... b.l ..
            if (h < b.l && l > b.h) {
                return false;
            }
            // ... l ... b.l ... h ...
            if (h >= b.l && l <= b.h) {
                result = b;
            } else if (h >= b.l) {
                result = interval(b.l, h, sz);
            } else {
                // ... l .. b.h .. h .. b.l ...
                SASSERT(l <= b.h);
                result = interval(l, std::min(h, b.h), sz);
            }
        } else {
            // 0 .. l.. l' ... h ... h'
            result = interval(std::max(l, b.l), std::min(h, b.h), sz);
        }
        return true;
    }

    /// return false if negation is empty
    bool negate(interval& result) const {
        if (is_full())
            return false;
        if (l.is_zero()) {
            result = interval(h + rational::one(), uMaxInt(sz), sz);
        } else if (uMaxInt(sz) == h) {
            result = interval(rational::zero(), l - rational::one(), sz);
        } else {
            result = interval(h + rational::one(), l - rational::one(), sz);
        }
        return true;
    }
};

std::ostream& operator<<(std::ostream& o, const interval& I) {
    o << "[" << I.l << ", " << I.h << "]";
    return o;
}


class bv_bounds_simplifier : public ctx_simplify_tactic::simplifier {
    ast_manager&                     m;
    bv_util                          m_bv;
    vector<obj_map<expr, interval> > m_scopes;
    obj_map<expr, interval>         *m_bound;

    bool is_bound(expr *e, expr*& v, interval& b) {
        rational n;
        expr *lhs, *rhs;
        unsigned sz;

        if (m_bv.is_bv_ule(e, lhs, rhs)) {
            if (m_bv.is_numeral(lhs, n, sz)) { // C ule x <=> x uge C
                b = interval(n, uMaxInt(sz), sz);
                v = rhs;
                return true;
            }
            if (m_bv.is_numeral(rhs, n, sz)) { // x ule C
                b = interval(rational::zero(), n, sz);
                v = lhs;
                return true;
            }
        } else if (m_bv.is_bv_sle(e, lhs, rhs)) {
            if (m_bv.is_numeral(lhs, n, sz)) { // C sle x <=> x sge C
                b = interval(n, rational::power_of_two(sz-1) - rational::one(), sz);
                v = rhs;
                return true;
            }
            if (m_bv.is_numeral(rhs, n, sz)) { // x sle C
                b = interval(rational::power_of_two(sz-1), n, sz);
                v = lhs;
                return true;
            }
        } else if (m.is_eq(e, lhs, rhs)) {
            if (m_bv.is_numeral(lhs, n, sz)) {
                b = interval(n, n, sz);
                v = rhs;
                return true;
            }
            if (m_bv.is_numeral(rhs, n, sz)) {
                b = interval(n, n, sz);
                v = lhs;
                return true;
            }
        }
        return false;
    }

    bool add_bound(expr* t, const interval& b) {
        push();
        interval& r = m_bound->insert_if_not_there2(t, b)->get_data().m_value;
        return r.intersect(b, r);
    }

public:

    bv_bounds_simplifier(ast_manager& m) : m(m), m_bv(m) {
        m_scopes.push_back(obj_map<expr, interval>());
        m_bound = &m_scopes.back();
    }

    virtual ~bv_bounds_simplifier() {}

    virtual void assert_expr(expr * t, bool sign) {
        interval b;
        expr* t1;
        if (is_bound(t, t1, b)) {
            if (sign)
                VERIFY(b.negate(b));

            TRACE("bv", tout << (sign?"(not ":"") << mk_pp(t, m) << (sign ? ")" : "") << ": " << mk_pp(t1, m) << " in " << b << "\n";);
            VERIFY(add_bound(t1, b));
        }
    }

    virtual bool simplify(expr* t, expr_ref& result) {
        expr* t1;
        interval b, ctx, intr;
        result = 0;
        if (!is_bound(t, t1, b))
            return false;

        if (m_bound->find(t1, ctx)) {
            if (!b.intersect(ctx, intr)) {
                result = m.mk_false();
            } else if (intr.l == intr.h) {
                result = m.mk_eq(t1, m_bv.mk_numeral(intr.l, m.get_sort(t1)));
            } else if (ctx.implies(b)) {
                result = m.mk_true();
            }
        }

        CTRACE("bv", result != 0, tout << mk_pp(t, m) << " " << b << " (ctx: " << ctx << ") (intr: " << intr << "): " << result << "\n";);
        return result != 0;
    }

    virtual void push() {
        TRACE("bv", tout << "push\n";);
        m_scopes.push_back(*m_bound);
        m_bound = &m_scopes.back();
    }

    virtual void pop(unsigned num_scopes) {
        TRACE("bv", tout << "pop: " << num_scopes << "\n";);
        m_scopes.shrink(m_scopes.size() - num_scopes);
        m_bound = &m_scopes.back();
    }

    virtual simplifier * translate(ast_manager & m) {
        return alloc(bv_bounds_simplifier, m);
    }

    virtual unsigned scope_level() const {
        return m_scopes.size() - 1;
    }
};

}

tactic * mk_bv_bounds_tactic(ast_manager & m, params_ref const & p) {
    return clean(alloc(ctx_simplify_tactic, m, alloc(bv_bounds_simplifier, m), p));
}
