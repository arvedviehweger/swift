// RUN: not --crash %target-swift-frontend %s -parse

typealias F = (inout Int?) -> Void

class C {
  var s: [String : Any?] = [:]
}

class K<T> {
  init(with: @escaping (T, F) -> Void) {}
}

_ = K{ (c: C?, fn: F) in fn(&(c.s["hi"])) }
