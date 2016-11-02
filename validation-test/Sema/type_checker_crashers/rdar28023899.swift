// RUN: not --crash %target-swift-frontend %s -parse
// REQUIRES: asserts

class B : Equatable {
  static func == (lhs: B, rhs: B) -> Bool { return true }
}

class C : B {
  static var v: C { return C() }
}

let c: C! = nil
_ = c == .v
