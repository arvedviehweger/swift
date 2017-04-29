//===--- TypeCheckSwitchStmt.cpp - Switch Exhaustiveness and Type Checks --===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file contains an algorithm for checking the exhaustiveness of switches.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Pattern.h"

#include <numeric>
#include <forward_list>

using namespace swift;

#define DEBUG_TYPE "TypeCheckSwitchStmt"

namespace {

  /// The SpaceEngine encapsulates an algorithm for computing the exhaustiveness
  /// of a switch statement using an algebra of spaces described by Fengyun Liu
  /// and an algorithm for computing warnings for pattern matching by
  //  Luc Maranget.
  ///
  /// The main algorithm centers around the computation of the difference and
  /// the intersection of the "Spaces" given in each case, which reduces the
  /// definition of exhaustiveness to checking if the difference of the space
  /// 'S' of the user's written patterns and the space 'T' of the pattern
  /// condition is empty.
  struct SpaceEngine {
    enum class SpaceKind : uint8_t {
      Empty           = 1 << 0,
      Type            = 1 << 1,
      Constructor     = 1 << 2,
      Disjunct        = 1 << 3,
      BooleanConstant = 1 << 4,
    };

    /// A data structure for conveniently pattern-matching on the kinds of
    /// two spaces.
    struct PairSwitch {
    public:
      constexpr PairSwitch(SpaceKind pair1, SpaceKind pair2)
        : Data(static_cast<uint8_t>(pair1) | (static_cast<uint8_t>(pair2) << 8))
        {}

      constexpr bool operator==(const PairSwitch other) const {
        return Data == other.Data;
      }

      constexpr operator int() const {
        return Data;
      }

    private:
      uint16_t Data;

      PairSwitch (const PairSwitch&) = delete;
      PairSwitch& operator= (const PairSwitch&) = delete;
    };

  #define PAIRCASE(XS, YS) case PairSwitch(XS, YS)

    class Space final {
    private:
      SpaceKind Kind;
      llvm::PointerIntPair<Type, 1, bool> TypeAndVal;
      Identifier Head;
      std::forward_list<Space> Spaces;

    public:
      explicit Space(Type T)
        : Kind(SpaceKind::Type), TypeAndVal(T, false), Head(Identifier()),
          Spaces({}){}
      explicit Space(Type T, Identifier H, SmallVectorImpl<Space> &SP)
        : Kind(SpaceKind::Constructor), TypeAndVal(T, false), Head(H),
          Spaces(SP.begin(), SP.end()) {}
      explicit Space(SmallVectorImpl<Space> &SP)
        : Kind(SpaceKind::Disjunct), TypeAndVal(Type(), false),
          Head(Identifier()), Spaces(SP.begin(), SP.end()) {}
      explicit Space()
        : Kind(SpaceKind::Empty), TypeAndVal(Type(), false), Head(Identifier()),
          Spaces({}) {}
      explicit Space(bool C)
        : Kind(SpaceKind::BooleanConstant), TypeAndVal(Type(), C),
          Head(Identifier()), Spaces({}) {}

      SpaceKind getKind() const { return Kind; }

      void dump() const LLVM_ATTRIBUTE_USED;

      bool isEmpty() const { return getKind() == SpaceKind::Empty; }

      Type getType() const {
        assert((getKind() == SpaceKind::Type
                || getKind() == SpaceKind::Constructor)
               && "Wrong kind of space tried to access space type");
        return TypeAndVal.getPointer();
      }

      Identifier getHead() const {
        assert(getKind() == SpaceKind::Constructor
               && "Wrong kind of space tried to access head");
        return Head;
      }

      const std::forward_list<Space> &getSpaces() const {
        assert((getKind() == SpaceKind::Constructor
                || getKind() == SpaceKind::Disjunct)
               && "Wrong kind of space tried to access subspace list");
        return Spaces;
      }

      bool getBoolValue() const {
        assert(getKind() == SpaceKind::BooleanConstant
                && "Wrong kind of space tried to access bool value");
        return TypeAndVal.getInt();
      }

      // An optimization that computes if the difference of this space and
      // another space is empty.
      bool isSubspace(const Space &other) const {
        if (this->isEmpty()) {
          return true;
        }

        if (other.isEmpty()) {
          return false;
        }

        switch (PairSwitch(getKind(), other.getKind())) {
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Empty):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Type):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Constructor):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::BooleanConstant): {
          // (S1 | ... | Sn) <= S iff (S1 <= S) && ... && (Sn <= S)
          for (auto &space : this->getSpaces()) {
            if (!space.isSubspace(other)) {
              return false;
            }
          }
          return true;
        }
        PAIRCASE (SpaceKind::Type, SpaceKind::Type): {
          // Optimization: Are the types equal?  If so, the space is covered.
          if (this->getType()->isEqual(other.getType())) {
            return true;
          }

          // (_ : Ty1) <= (_ : Ty2) iff D(Ty1) == D(Ty2)
          if (canDecompose(this->getType())) {
            SmallVector<Space, 4> disjuncts;
            decompose(this->getType(), disjuncts);
            Space or1Space(disjuncts);
            if (or1Space.isSubspace(other)) {
              return true;
            }
          }

          if (canDecompose(other.getType())) {
            SmallVector<Space, 4> disjuncts;
            decompose(other.getType(), disjuncts);
            Space or2Space(disjuncts);
            return this->isSubspace(or2Space);
          }

          return true;
        }
        PAIRCASE (SpaceKind::Type, SpaceKind::Disjunct): {
          // (_ : Ty1) <= (S1 | ... | Sn) iff (S1 <= S) || ... || (Sn <= S)
          for (auto &dis : other.getSpaces()) {
            if (this->isSubspace(dis)) {
              return true;
            }
          }

          // (_ : Ty1) <= (S1 | ... | Sn) iff D(Ty1) <= (S1 | ... | Sn)
          if (!canDecompose(this->getType())) {
            return false;
          }
          SmallVector<Space, 4> disjuncts;
          decompose(this->getType(), disjuncts);
          Space or1Space(disjuncts);
          return or1Space.isSubspace(other);
        }
        PAIRCASE (SpaceKind::Type, SpaceKind::Constructor): {
          // (_ : Ty1) <= H(p1 | ... | pn) iff D(Ty1) <= H(p1 | ... | pn)
          if (canDecompose(this->getType())) {
            SmallVector<Space, 4> disjuncts;
            decompose(this->getType(), disjuncts);
            Space or1Space(disjuncts);
            return or1Space.isSubspace(other);
          }
          // An undecomposable type is always larger than its constructor space.
          return false;
        }
        PAIRCASE (SpaceKind::Constructor, SpaceKind::Type):
          // Typechecking guaranteed this constructor is a subspace of the type.
          return true;
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Type):
          return other.getType()->isBool();
        PAIRCASE (SpaceKind::Constructor, SpaceKind::Constructor): {
          // Optimization: If the constructor heads don't match, subspace is
          // impossible.
          if (this->Head.compare(other.Head) != 0) {
            return false;
          }

          // Special Case: A constructor pattern may include the head but not
          // the payload patterns.  In that case the space is covered.
          if (other.getSpaces().empty()) {
            return true;
          }

          // H(a1, ..., an) <= H(b1, ..., bn) iff a1 <= b1 && ... && an <= bn
          auto i = this->getSpaces().begin();
          auto j = other.getSpaces().begin();
          for (; i != this->getSpaces().end() && j != other.getSpaces().end();
               ++i, ++j) {
            if (!(*i).isSubspace(*j)) {
              return false;
            }
          }
          return true;
        }
        PAIRCASE(SpaceKind::Constructor, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Disjunct): {
          // S <= (S1 | ... | Sn) <= S iff (S <= S1) || ... || (S <= Sn)
          for (auto &param : other.getSpaces()) {
            if (this->isSubspace(param)) {
              return true;
            }
          }
          return false;
        }

        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::BooleanConstant):
          return this->getBoolValue() == other.getBoolValue();

        PAIRCASE (SpaceKind::Empty, SpaceKind::BooleanConstant):
        PAIRCASE (SpaceKind::Constructor, SpaceKind::BooleanConstant):
        PAIRCASE (SpaceKind::Type, SpaceKind::BooleanConstant):
            return false;

        default:
          llvm_unreachable("Uncovered pair found while computing subspaces?");
        }
      }

      // Returns the intersection of this space with another.  The intersection
      // is the largest shared subspace occupied by both arguments.
      Space intersect(const Space &other) const {
        // The intersection of an empty space is empty.
        if (this->isEmpty() || other.isEmpty()) {
          return Space();
        }

        llvm::function_ref<Space(SmallVectorImpl<Space> &)> examineDecomp
          = [&](SmallVectorImpl<Space> &decomposition) -> Space {
          if (decomposition.empty()) {
            return Space();
          } else if (decomposition.size() == 1) {
            return decomposition.front();
          }
          Space ds(decomposition);
          return ds;
        };

        switch (PairSwitch(getKind(), other.getKind())) {
          PAIRCASE (SpaceKind::Empty, SpaceKind::Disjunct):
          PAIRCASE (SpaceKind::Type, SpaceKind::Disjunct):
          PAIRCASE (SpaceKind::Constructor, SpaceKind::Disjunct):
          PAIRCASE (SpaceKind::Disjunct, SpaceKind::Disjunct):
          PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Disjunct): {
            // S & (S1 || ... || Sn) iff (S & S1) && ... && (S & Sn)
            SmallVector<Space, 4> intersectedCases;
            std::transform(other.getSpaces().begin(), other.getSpaces().end(),
                           std::back_inserter(intersectedCases),
                           [&](const Space &s) {
              return this->intersect(s);
            });
            // Optimization: Remove all empty spaces.
            SmallVector<Space, 4> filteredCases;
            std::copy_if(intersectedCases.begin(), intersectedCases.end(),
                         std::back_inserter(filteredCases),
                         [&](const Space &s) {
              return !s.isEmpty();
            });
            return examineDecomp(filteredCases);
          }

          PAIRCASE (SpaceKind::Disjunct, SpaceKind::Empty):
          PAIRCASE (SpaceKind::Disjunct, SpaceKind::Type):
          PAIRCASE (SpaceKind::Disjunct, SpaceKind::Constructor):
          PAIRCASE (SpaceKind::Disjunct, SpaceKind::BooleanConstant): {
            // (S1 || ... || Sn) & S iff (S & S1) && ... && (S & Sn)
            SmallVector<Space, 4> intersectedCases;
            std::transform(this->getSpaces().begin(), this->getSpaces().end(),
                           std::back_inserter(intersectedCases),
                           [&](const Space &s) {
              return s.intersect(other);
            });
            // Optimization: Remove all empty spaces.
            SmallVector<Space, 4> filteredCases;
            std::copy_if(intersectedCases.begin(), intersectedCases.end(),
                         std::back_inserter(filteredCases),
                         [&](const Space &s) {
              return !s.isEmpty();
            });
            return examineDecomp(filteredCases);
          }
          PAIRCASE (SpaceKind::Type, SpaceKind::Type): {
            // Optimization: The intersection of equal types is that type.
            if (this->getType()->isEqual(other.getType())) {
              return other;
            } else if (canDecompose(this->getType())) {
              SmallVector<Space, 4> spaces;
              decompose(this->getType(), spaces);
              auto decomposition = examineDecomp(spaces);
              return decomposition.intersect(other);
            } else if (canDecompose(other.getType())) {
              SmallVector<Space, 4> spaces;
              decompose(other.getType(), spaces);
              auto disjunctSp = examineDecomp(spaces);
              return this->intersect(disjunctSp);
            } else {
              return other;
            }
          }
          PAIRCASE (SpaceKind::Type, SpaceKind::Constructor): {
            if (canDecompose(this->getType())) {
              SmallVector<Space, 4> spaces;
              decompose(this->getType(), spaces);
              auto decomposition = examineDecomp(spaces);
              return decomposition.intersect(other);
            } else {
              return other;
            }
          }
          PAIRCASE (SpaceKind::Constructor, SpaceKind::Type):
            return *this;
            
          PAIRCASE (SpaceKind::Constructor, SpaceKind::Constructor): {
            // Optimization: If the heads don't match, the intersection of
            // the constructor spaces is empty.
            if (this->getHead().compare(other.Head) != 0) {
              return Space();
            }

            // Special Case: A constructor pattern may include the head but not
            // the payload patterns.  In that case, the intersection is the
            // whole original space.
            if (other.getSpaces().empty()) {
              return *this;
            }

            SmallVector<Space, 4> paramSpace;
            auto i = this->getSpaces().begin();
            auto j = other.getSpaces().begin();
            for (; i != this->getSpaces().end() && j != other.getSpaces().end();
                 ++i, ++j) {
              auto intersection = (*i).intersect(*j);
              if (intersection.simplify().isEmpty()) {
                return Space();
              }
              paramSpace.push_back(intersection);
            }

            return examineDecomp(paramSpace);
          }

          PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::BooleanConstant):
            return this->getBoolValue() == other.getBoolValue() ? *this : Space();

          PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Type): {
            if (other.getType()->isBool()) {
              return this->getKind() == SpaceKind::BooleanConstant ? *this : Space();
            }

            if (canDecompose(other.getType())) {
              SmallVector<Space, 4> spaces;
              decompose(other.getType(), spaces);
              auto disjunctSp = examineDecomp(spaces);
              return this->intersect(disjunctSp);
            }
            return Space();
          }
          PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Empty):
          PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Constructor):
            return Space();

          PAIRCASE (SpaceKind::Type, SpaceKind::BooleanConstant): {
            // if (isSubType(tp2, tp1)) b
            if (canDecompose(this->getType())) {
              SmallVector<Space, 4> spaces;
              decompose(this->getType(), spaces);
              auto disjunctSp = examineDecomp(spaces);
              return disjunctSp.intersect(other);
            } else {
              return Space();
            }
          }

          PAIRCASE (SpaceKind::Empty, SpaceKind::BooleanConstant):
          PAIRCASE (SpaceKind::Constructor, SpaceKind::BooleanConstant):
            return Space();

          default:
            llvm_unreachable("Uncovered pair found while computing intersect?");
        }
      }

      // Returns the result of subtracting the other space from this space.  The
      // result is empty if the other space completely covers this space, or
      // non-empty if there were any uncovered cases.  The difference of spaces
      // is the smallest uncovered set of cases.
      Space minus(const Space &other) const {
        if (this->isEmpty()) {
          return Space();
        }

        if (other.isEmpty()) {
          return *this;
        }

        llvm::function_ref<Space(SmallVectorImpl<Space> &)> examineDecomp
          = [&](SmallVectorImpl<Space> &decomposition) -> Space {
          if (decomposition.empty()) {
            return Space();
          } else if (decomposition.size() == 1) {
            return decomposition.front();
          }
          return Space(decomposition);
        };

        switch (PairSwitch(this->getKind(), other.getKind())) {
        PAIRCASE (SpaceKind::Type, SpaceKind::Type): {
          // Optimization: Are the types equal?  If so, the space is covered.
          if (this->getType()->isEqual(other.getType())) {
            return Space();
          } else if (canDecompose(this->getType())) {
            SmallVector<Space, 4> spaces;
            this->decompose(this->getType(), spaces);
            return examineDecomp(spaces).intersect(other);
          } else if (canDecompose(other.getType())) {
            SmallVector<Space, 4> spaces;
            this->decompose(other.getType(), spaces);
            auto decomp = examineDecomp(spaces);
            return this->intersect(decomp);
          }
          return Space();
        }
        PAIRCASE (SpaceKind::Type, SpaceKind::Constructor): {
          if (canDecompose(this->getType())) {
            SmallVector<Space, 4> spaces;
            this->decompose(this->getType(), spaces);
            auto decomp = examineDecomp(spaces);
            return decomp.minus(other);
          } else {
            return *this;
          }
        }
        PAIRCASE (SpaceKind::Empty, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::Type, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::Constructor, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Disjunct):
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Disjunct): {
          return std::accumulate(other.getSpaces().begin(),
                                 other.getSpaces().end(),
                                 *this,
                                 [](const Space &left, const Space &right){
            return left.minus(right);
          });
        }

        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Empty):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Type):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::Constructor):
        PAIRCASE (SpaceKind::Disjunct, SpaceKind::BooleanConstant): {
          SmallVector<Space, 4> smallSpaces;
          std::transform(this->getSpaces().begin(), this->getSpaces().end(),
                         std::back_inserter(smallSpaces),
                         [&](const Space &first){
            return first.minus(other);
          });
          return examineDecomp(smallSpaces);
        }
        PAIRCASE (SpaceKind::Constructor, SpaceKind::Type):
          return Space();

        PAIRCASE (SpaceKind::Constructor, SpaceKind::Constructor): {
          // Optimization: If the heads of the constructors don't match then
          // the two are disjoint and their difference is the first space.
          if (this->Head.compare(other.Head) != 0) {
            return *this;
          }

          // Special Case: A constructor pattern may include the head but not
          // the payload patterns.  In that case, because the heads match, it
          // covers the whole space.
          if (other.getSpaces().empty()) {
            return Space();
          }

          SmallVector<Space, 4> constrSpaces;
          bool foundBad = false;
          auto i = this->getSpaces().begin();
          auto j = other.getSpaces().begin();
          for (auto idx = 0;
               i != this->getSpaces().end() && j != other.getSpaces().end();
               ++i, ++j, ++idx) {
            auto &s1 = *i;
            auto &s2 = *j;
            // If the intersection of each subspace is ever empty then the
            // two spaces are disjoint and their difference is the first space.
            if (s1.intersect(s2).simplify().isEmpty()) {
              return *this;
            }

            // If one constructor parameter doens't cover the other then we've
            // got to report the uncovered cases in a user-friendly way.
            if (!s1.isSubspace(s2)) {
              foundBad = true;
            }
            // Copy the params and replace the parameter at each index with the
            // difference of the two spaces.  This unpacks one constructor head
            // into each parameter.
            SmallVector<Space, 4> copyParams(this->getSpaces().begin(),
                                             this->getSpaces().end());
            copyParams[idx] = s1.minus(s2);
            Space CS(this->getType(), this->Head, copyParams);
            constrSpaces.push_back(CS);
          }

          if (foundBad) {
            return examineDecomp(constrSpaces);
          }
          return Space();
        }
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::BooleanConstant): {
          // The difference of boolean constants depends on their values.
          if (this->getBoolValue() == other.getBoolValue()) {
            return Space();
          } else {
            return *this;
          }
        }
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Type): {
          if (other.getType()->isBool()) {
            return (getKind() == SpaceKind::BooleanConstant)  ? Space() : *this;
          }

          if (canDecompose(other.getType())) {
            SmallVector<Space, 4> spaces;
            this->decompose(other.getType(), spaces);
            auto disjunctSp = examineDecomp(spaces);
            return this->minus(disjunctSp);
          }
          return *this;
        }
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Empty):
        PAIRCASE (SpaceKind::BooleanConstant, SpaceKind::Constructor):
          return *this;

        PAIRCASE (SpaceKind::Type, SpaceKind::BooleanConstant): {
          if (canDecompose(this->getType())) {
            SmallVector<Space, 4> spaces;
            this->decompose(this->getType(), spaces);
            auto orSpace = examineDecomp(spaces);
            return orSpace.minus(other);
          } else {
            return *this;
          }
        }

        PAIRCASE (SpaceKind::Empty, SpaceKind::BooleanConstant):
        PAIRCASE (SpaceKind::Constructor, SpaceKind::BooleanConstant):
          return Space();
        default:
          llvm_unreachable("Uncovered pair found while computing difference?");
        }
      }

      void show(llvm::raw_ostream &buffer, bool normalize = true) const {
        switch (getKind()) {
        case SpaceKind::Empty:
          buffer << "[EMPTY]";
          break;
        case SpaceKind::Disjunct: {
          if (normalize) {
            return simplify().show(buffer, false);
          } else {
            buffer << "DISJOIN(";
            for (auto &sp : Spaces) {
              buffer << "\n";
              sp.show(buffer, normalize);
              buffer << " |";
            }
            buffer << ")";
          }
        }
          break;
        case SpaceKind::BooleanConstant:
          buffer << ((getBoolValue()) ? "true" : "false");
          break;
        case SpaceKind::Constructor: {
          if (!Head.empty()) {
            buffer << ".";
            buffer << Head.str();
          }

          if (Spaces.empty()) {
            return;
          }

          buffer << "(";
          bool first = true;
          for (auto &param : Spaces) {
            if (!first) {
              buffer << ", ";
            }
            if (normalize) {
              param.simplify().show(buffer, normalize);
            } else {
              param.show(buffer);
            }
            if (first) {
              first = false;
            }
          }
          buffer << ")";
        }
          break;
        case SpaceKind::Type:
          if (!normalize) {
            getType()->print(buffer);
          }
          buffer << "_";
          break;
        }
      }

      // For optimization, attempt to simplify a space by removing any empty
      // cases and unpacking empty or singular disjunctions where possible.
      Space simplify() const {
        switch (getKind()) {
        case SpaceKind::Constructor: {
          // If a constructor has no spaces it is an enum without a payload and
          // cannot be optimized further.
          if (getSpaces().empty()) {
            return *this;
          }

          // Simplify each component subspace.  If, after simplification, any
          // subspace contains an empty, then the whole space is empty.
          SmallVector<Space, 4> simplifiedSpaces;
          std::transform(getSpaces().begin(), getSpaces().end(),
                         std::back_inserter(simplifiedSpaces),
                         [](const Space &el) {
            return el.simplify();
          });
          for (auto &el : simplifiedSpaces) {
            if (el.isEmpty()) {
              return Space();
            }
          }
          return Space(getType(), Head, simplifiedSpaces);
        }
        case SpaceKind::Type: {
          // If the decomposition of a space is empty, the space is empty.
          if (canDecompose(this->getType())) {
            SmallVector<Space, 4> ss;
            decompose(this->getType(), ss);
            if (ss.empty()) {
              return Space();
            }
            return *this;
          } else {
            return *this;
          }
        }
        case SpaceKind::Disjunct: {
          // Simplify each disjunct.
          SmallVector<Space, 4> simplifiedSpaces;
          std::transform(Spaces.begin(), Spaces.end(),
                         std::back_inserter(simplifiedSpaces),
                         [](const Space &el){
            return el.simplify();
          });
          // If the disjunct is singular, unpack it into its component.
          if (simplifiedSpaces.size() == 1) {
            return simplifiedSpaces.front();
          }

          // Otherwise, remove any empties.
          SmallVector<Space, 4> compatifiedSpaces;
          std::copy_if(simplifiedSpaces.begin(), simplifiedSpaces.end(),
                       std::back_inserter(compatifiedSpaces),
                       [&](const Space &el) {
            return !el.isEmpty();
          });
          // If the disjunct was all empty, the space is empty.
          if (compatifiedSpaces.empty()) {
            return Space();
          }
          // Else if the disjunct is singular, unpack it into its component.
          if (compatifiedSpaces.size() == 1) {
            return compatifiedSpaces.front();
          }
          return Space(compatifiedSpaces);
        }
        default:
          return *this;
        }
      }

      // Decompose a type into its component spaces.
      static void decompose(Type tp, SmallVectorImpl<Space> &arr) {
        assert(canDecompose(tp) && "Non-decomposable type?");

        if (tp->isBool()) {
          arr.push_back(Space(true));
          arr.push_back(Space(false));
        } else if (auto *E = tp->getEnumOrBoundGenericEnum()) {
          // Look into each case of the enum and decompose it in turn.
          auto children = E->getAllElements();
          std::transform(children.begin(), children.end(),
                         std::back_inserter(arr), [&](EnumElementDecl *eed) {
            // FIXME: This shouldn't happen.
            if (!eed->hasInterfaceType()) {
              return Space();
            }
            auto eedTy = tp->getCanonicalType()
                           ->getTypeOfMember(E->getModuleContext(), eed,
                                             eed->getArgumentInterfaceType());
            SmallVector<Space, 4> constElemSpaces;
            if (eedTy) {
              if (auto *TTy = eedTy->getAs<TupleType>()) {
                // Decompose the payload tuple into its component type spaces.
                std::transform(TTy->getElements().begin(),
                               TTy->getElements().end(),
                               std::back_inserter(constElemSpaces),
                               [&](TupleTypeElt ty){
                                 return Space(ty.getType());
                               });
              } else if (auto *TTy = dyn_cast<ParenType>(eedTy.getPointer())) {
                constElemSpaces.push_back(Space(TTy->getUnderlyingType()));
              }
            }
            return Space(tp, eed->getName(), constElemSpaces);
          });
        } else if (auto *TTy = tp->castTo<TupleType>()) {
          // Decompose each of the elements into its component type space.
          SmallVector<Space, 4> constElemSpaces;
          std::transform(TTy->getElements().begin(), TTy->getElements().end(),
                         std::back_inserter(constElemSpaces),
                         [&](TupleTypeElt ty){
            return Space(ty.getType());
          });
          // Create an empty constructor head for the tuple space.
          arr.push_back(Space(tp, Identifier(), constElemSpaces));
        } else {
          llvm_unreachable("Can't decompose type?");
        }
      }

      static bool canDecompose(Type tp) {
        return tp->is<TupleType>() || tp->isBool()
            || tp->getEnumOrBoundGenericEnum();
      }
    };

    ASTContext &Ctx;
    SwitchStmt *Switch;

    SpaceEngine(ASTContext &C, SwitchStmt *SS) : Ctx(C), Switch(SS) {}

    void checkExhaustiveness(bool limitedChecking) {
      if (limitedChecking) {
        // Reject switch statements with empty blocks.
        if (Switch->getCases().empty())
          SpaceEngine::diagnoseMissingCases(Ctx, Switch,
                                            /*justNeedsDefault*/true,
                                            SpaceEngine::Space());
        return;
      }

      SmallVector<Space, 4> spaces;
      for (unsigned i = 0, e = Switch->getCases().size(); i < e; ++i) {
        auto *caseBlock = Switch->getCases()[i];
        for (auto &caseItem : caseBlock->getCaseLabelItems()) {
          // 'where'-clauses on cases mean the case does not contribute to
          // the exhaustiveness of the pattern.
          if (caseItem.getGuardExpr())
            continue;

          auto projection = projectPattern(Ctx, caseItem.getPattern());

          // Space is trivially covered with a default clause.
          if (caseItem.isDefault())
            return;
          spaces.push_back(projection);
        }
      }

      Space totalSpace(Switch->getSubjectExpr()->getType());
      Space coveredSpace(spaces);
      auto uncovered = totalSpace.minus(coveredSpace).simplify();
      if (uncovered.isEmpty()) {
        return;
      }

      // If the entire space is left uncovered we have two choices: We can
      // decompose the type space and offer them as fixits, or simply offer
      // to insert a `default` clause.
      if (uncovered.getKind() == SpaceKind::Type) {
        if (Space::canDecompose(uncovered.getType())) {
          SmallVector<Space, 4> spaces;
          Space::decompose(uncovered.getType(), spaces);
          diagnoseMissingCases(Ctx, Switch,
                               /*justNeedsDefault*/ false, Space(spaces));
        } else {
          diagnoseMissingCases(Ctx, Switch,
                               /*justNeedsDefault*/ true, Space());
        }
        return;
      }

      // If the space isn't a disjunct then make it one.
      if (uncovered.getKind() != SpaceKind::Disjunct) {
        SmallVector<Space, 1> spaces = { uncovered };
        uncovered = Space(spaces);
      }

      diagnoseMissingCases(Ctx, Switch, /*justNeedsDefault*/ false, uncovered);
    }

    static void diagnoseMissingCases(ASTContext &Ctx, const SwitchStmt *Switch,
                                     bool JustNeedsDefault,
                                     SpaceEngine::Space uncovered) {
      bool Empty = Switch->getCases().empty();
      SourceLoc StartLoc = Switch->getStartLoc();
      SourceLoc EndLoc = Switch->getEndLoc();
      StringRef Placeholder = getCodePlaceholder();
      llvm::SmallString<128> Buffer;
      llvm::raw_svector_ostream OS(Buffer);

      bool InEditor = Ctx.LangOpts.DiagnosticsEditorMode;

      if (JustNeedsDefault) {
        OS << tok::kw_default << ": " << Placeholder << "\n";
        if (Empty) {
          Ctx.Diags.diagnose(StartLoc, diag::empty_switch_stmt)
             .fixItInsert(EndLoc, Buffer.str());
        } else {
          Ctx.Diags.diagnose(StartLoc, diag::non_exhaustive_switch,
                             InEditor, uncovered.isEmpty(), "")
             .fixItInsert(EndLoc, Buffer.str());
        }
        return;
      }

      // If there's nothing else to diagnose, bail.
      if (uncovered.isEmpty()) return;

      // If editing is enabled, emit a formatted error of the form:
      //
      // switch must be exhaustive, do you want to add missing cases?
      //     case (.none, .some(_)): <#code#>
      //     case (.some(_), .none): <#code#>
      //
      // else:
      //
      // switch must be exhaustive, consider adding missing cases:
      //
      // missing case '(.none, .some(_))'
      // missing case '(.some(_), .none)'
      if (InEditor) {
        Buffer.clear();
        for (auto &uncoveredSpace : uncovered.getSpaces()) {
          SmallVector<Space, 4> flats;
          flatten(uncoveredSpace, flats);
          if (flats.empty()) {
            flats.append({ uncoveredSpace });
          }
          for (size_t i = 0; i < flats.size(); ++i) {
            auto &FlatSpace = flats[i];
            OS << tok::kw_case << " ";
            FlatSpace.show(OS);
            OS << ": " << Placeholder << "\n";
          }
        }

        auto Diag = Ctx.Diags.diagnose(StartLoc, diag::non_exhaustive_switch,
                                       InEditor, false, Buffer.str());
        Diag.fixItInsert(EndLoc, Buffer.str());
      } else {
        Ctx.Diags.diagnose(StartLoc, diag::non_exhaustive_switch,
                           InEditor, false, Buffer.str());

        for (auto &uncoveredSpace : uncovered.getSpaces()) {
          SmallVector<Space, 4> flats;
          flatten(uncoveredSpace, flats);
          if (flats.empty()) {
            flats.append({ uncoveredSpace });
          }
          for (size_t i = 0; i < flats.size(); ++i) {
            Buffer.clear();
            auto &FlatSpace = flats[i];
            FlatSpace.show(OS);
            Ctx.Diags.diagnose(StartLoc, diag::missing_particular_case,
                               Buffer.str());
          }
        }
      }
    }

  private:
    // Recursively unpacks a space of disjunctions or constructor parameters
    // into its component parts such that the resulting array of flattened
    // spaces contains no further disjunctions.  If there were no disjunctions
    // in the starting space, the original space is already flat and the
    // returned array of spaces will be empty.
    static void flatten(const Space space, SmallVectorImpl<Space> &flats) {
      switch (space.getKind()) {
      case SpaceKind::Constructor: {
        size_t i = 0;
        for (auto &param : space.getSpaces()) {
          // We're only interested in recursively unpacking constructors and
          // disjunctions (booleans are considered constructors).
          if (param.getKind() != SpaceKind::Constructor
              || param.getKind() != SpaceKind::Disjunct
              || param.getKind() != SpaceKind::BooleanConstant)
            continue;

          SmallVector<Space, 4> flattenedParams;
          flatten(param, flattenedParams);
          for (auto &space : flattenedParams) {
            SmallVector<Space, 4> row(space.getSpaces().begin(),
                                      space.getSpaces().end());
            row[i] = space;
            Space cs(space.getType(), space.getHead(), row);
            flats.push_back(cs);
          }
          ++i;
        }
      }
        break;
      case SpaceKind::Disjunct: {
        auto begin = space.getSpaces().begin();
        auto end = space.getSpaces().end();
        for (; begin != end; ++begin) {
          flatten(*begin, flats);
        }
      }
        break;
      default:
        flats.push_back(space);
        break;
      }
    }

    // Recursively project a pattern into a Space.
    static Space projectPattern(ASTContext &Ctx, const Pattern *item) {
      switch (item->getKind()) {
      case PatternKind::Any:
      case PatternKind::Named:
        return Space(item->getType());
      case PatternKind::Bool: {
        auto *BP = cast<BoolPattern>(item);
        return Space(BP->getValue());
      }
      case PatternKind::Typed:
      case PatternKind::Is:
      case PatternKind::Expr:
        return Space();
      case PatternKind::Var: {
        auto *VP = cast<VarPattern>(item);
        return projectPattern(Ctx, VP->getSubPattern());
      }
      case PatternKind::Paren: {
        auto *PP = cast<ParenPattern>(item);
        return projectPattern(Ctx, PP->getSubPattern());
      }
      case PatternKind::OptionalSome: {
        auto *OSP = cast<OptionalSomePattern>(item);
        SmallVector<Space, 1> payload = {
          projectPattern(Ctx, OSP->getSubPattern())
        };
        return Space(item->getType(), Ctx.getIdentifier("some"), payload);
      }
      case PatternKind::EnumElement: {
        auto *VP = cast<EnumElementPattern>(item);
        SmallVector<Space, 4> conArgSpace;
        auto *SP = VP->getSubPattern();
        if (!SP) {
          // If there's no sub-pattern then there's no further recursive
          // structure here.  Yield the constructor space.
          return Space(item->getType(), VP->getName(), conArgSpace);
        }

        switch (SP->getKind()) {
        case PatternKind::Tuple: {
          auto *TP = dyn_cast<TuplePattern>(SP);
          std::transform(TP->getElements().begin(), TP->getElements().end(),
                         std::back_inserter(conArgSpace),
                         [&](TuplePatternElt pate) {
                           return projectPattern(Ctx, pate.getPattern());
                         });
          return Space(item->getType(), VP->getName(), conArgSpace);
        }
        case PatternKind::Paren: {
          auto *PP = dyn_cast<ParenPattern>(SP);
          auto *SP = PP->getSemanticsProvidingPattern();

          // Special Case: A constructor pattern may have all of its payload
          // matched by a single var pattern.  Project it like the tuple it
          // really is.
          if (SP->getKind() == PatternKind::Named
              || SP->getKind() == PatternKind::Any
              || SP->getKind() == PatternKind::Tuple) {
            if (auto *TTy = SP->getType()->getAs<TupleType>()) {
              for (auto ty : TTy->getElements()) {
                conArgSpace.push_back(Space(ty.getType()));
              }
            } else {
              conArgSpace.push_back(projectPattern(Ctx, SP));
            }
          } else {
            conArgSpace.push_back(projectPattern(Ctx, SP));
          }
          return Space(item->getType(), VP->getName(), conArgSpace);
        }
        default:
          return projectPattern(Ctx, SP);
        }
      }
      case PatternKind::Tuple: {
        auto *TP = cast<TuplePattern>(item);
        SmallVector<Space, 4> conArgSpace;
        std::transform(TP->getElements().begin(), TP->getElements().end(),
                       std::back_inserter(conArgSpace),
                       [&](TuplePatternElt pate) {
          return projectPattern(Ctx, pate.getPattern());
        });
        return Space(item->getType(), Identifier(), conArgSpace);
      }
      }
    }
  };
} // end anonymous namespace

void TypeChecker::checkSwitchExhaustiveness(SwitchStmt *stmt, bool limited) {
  SpaceEngine(Context, stmt).checkExhaustiveness(limited);
}

void SpaceEngine::Space::dump() const {
  SmallString<128> buf;
  llvm::raw_svector_ostream os(buf);
  this->show(os, /*normalize*/false);
  llvm::errs() << buf.str();
  llvm::errs() << "\n";
}
