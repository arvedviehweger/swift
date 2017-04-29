// RUN: %target-typecheck-verify-swift
func foo(a: Int?, b: Int?) -> Int {
  switch (a, b) {
  case (.none, _): return 1
  case (_, .none): return 2
  case (.some(_), .some(_)): return 3
  }
}

func bar(a: Bool, b: Bool) -> Int {
  switch (a, b) {
  case (false, false):
    return 1
  case (true, _):
    return 2
  case (false, true):
    return 3
  }
}


enum Result<T> {
  case Ok(T)
  case Error(Error)

  func shouldWork<U>(other: Result<U>) -> Int {
    switch (self, other) { // No warning
    case (.Ok, .Ok): return 1
    case (.Error, .Error): return 2
    case (.Error, _): return 3
    case (_, .Error): return 4
    }
  }
}

enum Foo {
  case A(Int)
  case B(Int)
}
func foo() {
  switch (Foo.A(1), Foo.B(1)) {
  case (.A(_), .A(_)):
    ()
  case (.B(_), _):
    ()
  case (_, .B(_)):
    ()
  }
}

class C {}

enum Bar {
  case TheCase(C?)
}

func test(f: Bar) -> Bool {
  switch f {
  case .TheCase(_?):
    return true
  case .TheCase(nil):
    return false
  }
}

func op(this : Optional<Bool>, other : Optional<Bool>) -> Optional<Bool> {
  switch (this, other) { // No warning
  case let (.none, w):
    return w
  case let (w, .none):
    return w
  case let (.some(e1), .some(e2)):
    return .some(e1 && e2)
  }
}

enum Threepeat {
  case a, b, c
}

func test3(x: Threepeat, y: Threepeat) {
  switch (x, y) { // expected-error {{switch must be exhaustive, consider adding missing cases:}}
  // expected-note@-1 {{missing case: '(.a, .c)'}}
  case (.a, .a):
    ()
  case (.b, _):
    ()
  case (.c, _):
    ()
  case (_, .b):
    ()
  }
}

enum A {
  case A(Int)
  case B(Bool)
  case C
  case D
}

enum B {
  case A
  case B
}

func s(a: A, b: B) {
  switch (a, b) {
  case (.A(_), .A):
    break
  case (.A(_), .B):
    break

  case (.B(_), let b):
  // expected-warning@-1 {{immutable value 'b' was never used; consider replacing with '_' or removing it}}
    break

  case (.C, _), (.D, _):
    break
  }
}


enum Grimble {
  case A
  case B
  case C
}

enum Gromble {
  case D
  case E
}

func doSomething(foo:Grimble, bar:Gromble) {
  switch(foo, bar) { // No warning
  case (.A, .D):
    break
  case (.A, .E):
    break
  case (.B, _):
    break
  case (.C, _):
    break
  }
}

enum E {
  case A
  case B
}

func f(l: E, r: E) {
  switch (l, r) {
  case (.A, .A):
    return
  case (.A, _):
    return
  case (_, .A):
    return
  case (.B, .B):
    return
  }
}

enum TestEnum {
  case A, B
}

func switchOverEnum(testEnumTuple: (TestEnum, TestEnum)) {

  switch testEnumTuple {
  case (_,.B):
    // Matches (.A, .B) and (.B, .B)
    break
  case (.A,_):
    // Matches (.A, .A)
    // Would also match (.A, .B) but first case takes precedent
    break
  case (.B,.A):
    // Matches (.B, .A)
    break
  }

}

func tests(a: Int?, b: String?) {
  switch (a, b) {
  case let (.some(n), _): print("a: ", n, "?")
  case (.none, _): print("Nothing", "?")
  }

  switch (a, b) {
  case let (.some(n), .some(s)): print("a: ", n, "b: ", s)
  case let (.some(n), .none): print("a: ", n, "Nothing")
  case (.none, _): print("Nothing")
  }

  switch (a, b) {
  case let (.some(n), .some(s)): print("a: ", n, "b: ", s)
  case let (.some(n), .none): print("a: ", n, "Nothing")
  case let (.none, .some(s)): print("Nothing", "b: ", s)
  case (.none, _): print("Nothing", "?")
  }

  switch (a, b) {
  case let (.some(n), .some(s)): print("a: ", n, "b: ", s)
  case let (.some(n), .none): print("a: ", n, "Nothing")
  case let (.none, .some(s)): print("Nothing", "b: ", s)
  case (.none, .none): print("Nothing", "Nothing")
  }

}

enum X {
  case Empty
  case A(Int)
  case B(Int)
}

func f(a: X, b: X) {
  switch (a, b) {
  case (_, .Empty): ()
  case (.Empty, _): ()

  case (.A, .A): ()
  case (.B, .B): ()

  case (.A, .B): ()
  case (.B, .A): ()

  }
}

// FIXME: Duplicate case warning is not caught by dataflow diagnostics.
//func f2(a: X, b: X) {
//  switch (a, b) {
//
//  case (.A, .A): ()
//  case (.B, .B): ()
//
//  case (.A, .B): ()
//  case (.B, .A): ()
//
//  case (_, .Empty): ()
//  case (.Empty, _): ()
//
//  // duplicate cases, but no warning? (comment out the line above and warnings appear!?)
//  case (.A, .A): ()
//  case (.B, .B): ()
//
//  case (.A, .B): ()
//  case (.B, .A): ()
//
//  default: ()
//  }
//}

enum XX : Int {
  case A
  case B
  case C
  case D
  case E
}

func switcheroo(a: XX, b: XX) -> Int {
  switch(a, b) { // No warning
  case (.A, _)  : return 1
  case (_, .A)  : return 2

  case (.C, _)  : return 3
  case (_, .C)  : return 4

  case (.B, .B) : return 5
  case (.B, .D) : return 6
  case (.D, .B) : return 7

  case (.B, .E) : return 8
  case (.E, .B) : return 9

  case (.E, _)  : return 10
  case (_, .E)  : return 11
  case (.D, .D) : return 12

  default:
    print("never hits this:", a, b)
    return 13
  }
}
