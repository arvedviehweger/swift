// RUN: %target-swift-frontend -enable-experimental-keypaths -emit-silgen %s | %FileCheck %s
// REQUIRES: PTRSIZE=64

struct S<T> {
  var x: T
  let y: String
  var z: C<T>

  var computed: C<T> { fatalError() }
  var observed: C<T> { didSet { fatalError() } }
  var reabstracted: () -> ()
}
class C<T> {
  final var x: T
  final let y: String
  final var z: S<T>

  var nonfinal: S<T>
  var computed: S<T> { fatalError() }
  var observed: S<T> { didSet { fatalError() } }
  final var reabstracted: () -> ()

  init() { fatalError() }
}

protocol P {
  var x: Int { get }
  var y: String { get set }
}

// CHECK-LABEL: sil hidden @{{.*}}storedProperties
func storedProperties<T>(_: T) {
  // CHECK: keypath $WritableKeyPath<S<T>, T>, <τ_0_0> (root $S<τ_0_0>; stored_property #S.x : $τ_0_0) <T>
  _ = #keyPath2(S<T>, .x)
  // CHECK: keypath $KeyPath<S<T>, String>, <τ_0_0> (root $S<τ_0_0>; stored_property #S.y : $String) <T>
  _ = #keyPath2(S<T>, .y)
  // CHECK: keypath $ReferenceWritableKeyPath<S<T>, T>, <τ_0_0> (root $S<τ_0_0>; stored_property #S.z : $C<τ_0_0>; stored_property #C.x : $τ_0_0) <T>
  _ = #keyPath2(S<T>, .z.x)
  // CHECK: keypath $ReferenceWritableKeyPath<C<T>, T>, <τ_0_0> (root $C<τ_0_0>; stored_property #C.x : $τ_0_0) <T>
  _ = #keyPath2(C<T>, .x)
  // CHECK: keypath $KeyPath<C<T>, String>, <τ_0_0> (root $C<τ_0_0>; stored_property #C.y : $String) <T>
  _ = #keyPath2(C<T>, .y)
  // CHECK: keypath $ReferenceWritableKeyPath<C<T>, T>, <τ_0_0> (root $C<τ_0_0>; stored_property #C.z : $S<τ_0_0>; stored_property #S.x : $τ_0_0) <T>
  _ = #keyPath2(C<T>, .z.x)
  // CHECK: keypath $KeyPath<C<T>, String>, <τ_0_0> (root $C<τ_0_0>; stored_property #C.z : $S<τ_0_0>; stored_property #S.z : $C<τ_0_0>; stored_property #C.y : $String) <T>
  _ = #keyPath2(C<T>, .z.z.y)
}

// CHECK-LABEL: sil hidden @{{.*}}computedProperties
func computedProperties<T: P>(_: T) {
  // CHECK: keypath $ReferenceWritableKeyPath<C<T>, S<T>>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $C<τ_0_0>;
  // CHECK-SAME: settable_property $S<τ_0_0>, 
  // CHECK-SAME:   id #C.nonfinal!getter.1 : <T> (C<T>) -> () -> S<T>,
  // CHECK-SAME:   getter @_T08keypaths1CC8nonfinalAA1SVyxGvTK : $@convention(thin) <τ_0_0> (@in C<τ_0_0>, @thick C<τ_0_0>.Type) -> @out S<τ_0_0>,
  // CHECK-SAME:   setter @_T08keypaths1CC8nonfinalAA1SVyxGvTk : $@convention(thin) <τ_0_0> (@in S<τ_0_0>, @in C<τ_0_0>, @thick C<τ_0_0>.Type) -> ()
  // CHECK-SAME: ) <T>
  _ = #keyPath2(C<T>, .nonfinal)

  // CHECK: keypath $KeyPath<C<T>, S<T>>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $C<τ_0_0>;
  // CHECK-SAME: gettable_property $S<τ_0_0>,
  // CHECK-SAME:   id #C.computed!getter.1 : <T> (C<T>) -> () -> S<T>,
  // CHECK-SAME:   getter @_T08keypaths1CC8computedAA1SVyxGvTK : $@convention(thin) <τ_0_0> (@in C<τ_0_0>, @thick C<τ_0_0>.Type) -> @out S<τ_0_0>
  // CHECK-SAME: ) <T>
  _ = #keyPath2(C<T>, .computed)

  // CHECK: keypath $ReferenceWritableKeyPath<C<T>, S<T>>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $C<τ_0_0>;
  // CHECK-SAME: settable_property $S<τ_0_0>, 
  // CHECK-SAME:   id #C.observed!getter.1 : <T> (C<T>) -> () -> S<T>,
  // CHECK-SAME:   getter @_T08keypaths1CC8observedAA1SVyxGvTK : $@convention(thin) <τ_0_0> (@in C<τ_0_0>, @thick C<τ_0_0>.Type) -> @out S<τ_0_0>,
  // CHECK-SAME:   setter @_T08keypaths1CC8observedAA1SVyxGvTk : $@convention(thin) <τ_0_0> (@in S<τ_0_0>, @in C<τ_0_0>, @thick C<τ_0_0>.Type) -> ()
  // CHECK-SAME: ) <T>
  _ = #keyPath2(C<T>, .observed)

  _ = #keyPath2(C<T>, .nonfinal.x)
  _ = #keyPath2(C<T>, .computed.x)
  _ = #keyPath2(C<T>, .observed.x)
  _ = #keyPath2(C<T>, .z.computed)
  _ = #keyPath2(C<T>, .z.observed)
  _ = #keyPath2(C<T>, .observed.x)

  // CHECK: keypath $ReferenceWritableKeyPath<C<T>, () -> ()>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $C<τ_0_0>;
  // CHECK-SAME: settable_property $() -> (), 
  // CHECK-SAME:   id ##C.reabstracted,
  // CHECK-SAME:   getter @_T08keypaths1CC12reabstractedyycvTK : $@convention(thin) <τ_0_0> (@in C<τ_0_0>, @thick C<τ_0_0>.Type) -> @out @callee_owned (@in ()) -> @out (),
  // CHECK-SAME:   setter @_T08keypaths1CC12reabstractedyycvTk : $@convention(thin) <τ_0_0> (@in @callee_owned (@in ()) -> @out (), @in C<τ_0_0>, @thick C<τ_0_0>.Type) -> ()
  // CHECK-SAME: ) <T>
  _ = #keyPath2(C<T>, .reabstracted)

  // CHECK: keypath $KeyPath<S<T>, C<T>>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $S<τ_0_0>; gettable_property $C<τ_0_0>,
  // CHECK-SAME: id @_T08keypaths1SV8computedAA1CCyxGfg : $@convention(method) <τ_0_0> (@in_guaranteed S<τ_0_0>) -> @owned C<τ_0_0>,
  // CHECK-SAME:   getter @_T08keypaths1SV8computedAA1CCyxGvTK : $@convention(thin) <τ_0_0> (@in S<τ_0_0>, @thick S<τ_0_0>.Type) -> @out C<τ_0_0>
  // CHECK-SAME: ) <T>
  _ = #keyPath2(S<T>, .computed)

  // CHECK: keypath $WritableKeyPath<S<T>, C<T>>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $S<τ_0_0>;
  // CHECK-SAME: settable_property $C<τ_0_0>,
  // CHECK-SAME:   id @_T08keypaths1SV8observedAA1CCyxGfg : $@convention(method) <τ_0_0> (@in_guaranteed S<τ_0_0>) -> @owned C<τ_0_0>,
  // CHECK-SAME:   getter @_T08keypaths1SV8observedAA1CCyxGvTK : $@convention(thin) <τ_0_0> (@in S<τ_0_0>, @thick S<τ_0_0>.Type) -> @out C<τ_0_0>,
  // CHECK-SAME:   setter @_T08keypaths1SV8observedAA1CCyxGvTk : $@convention(thin) <τ_0_0> (@in C<τ_0_0>, @inout S<τ_0_0>, @thick S<τ_0_0>.Type) -> ()
  // CHECK-SAME: ) <T>
  _ = #keyPath2(S<T>, .observed)
  _ = #keyPath2(S<T>, .z.nonfinal)
  _ = #keyPath2(S<T>, .z.computed)
  _ = #keyPath2(S<T>, .z.observed)
  _ = #keyPath2(S<T>, .computed.x)
  _ = #keyPath2(S<T>, .computed.y)
  // CHECK: keypath $WritableKeyPath<S<T>, () -> ()>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME:  root $S<τ_0_0>;
  // CHECK-SAME:  settable_property $() -> (),
  // CHECK-SAME:    id ##S.reabstracted,
  // CHECK-SAME:    getter @_T08keypaths1SV12reabstractedyycvTK : $@convention(thin) <τ_0_0> (@in S<τ_0_0>, @thick S<τ_0_0>.Type) -> @out @callee_owned (@in ()) -> @out (),
  // CHECK-SAME:    setter @_T08keypaths1SV12reabstractedyycvTk : $@convention(thin) <τ_0_0> (@in @callee_owned (@in ()) -> @out (), @inout S<τ_0_0>, @thick S<τ_0_0>.Type) -> ()
  // CHECK-SAME: ) <T>
  _ = #keyPath2(S<T>, .reabstracted)

  // CHECK: keypath $KeyPath<T, Int>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $τ_0_0;
  // CHECK-SAME: gettable_property $Int, 
  // CHECK-SAME:   id #P.x!getter.1 : <Self where Self : P> (Self) -> () -> Int,
  // CHECK-SAME:   getter @_T08keypaths1PP1xSivTK : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@in τ_0_0) -> @out Int
  // CHECK-SAME: ) <T>
  _ = #keyPath2(T, .x)
  // CHECK: keypath $WritableKeyPath<T, String>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $τ_0_0;
  // CHECK-SAME: settable_property $String,
  // CHECK-SAME:   id #P.y!getter.1 : <Self where Self : P> (Self) -> () -> String,
  // CHECK-SAME:   getter @_T08keypaths1PP1ySSvTK : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@in τ_0_0) -> @out String,
  // CHECK-SAME:   setter @_T08keypaths1PP1ySSvTk : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@in String, @inout τ_0_0) -> ()
  // CHECK-SAME: ) <T>
  _ = #keyPath2(T, .y)
}
