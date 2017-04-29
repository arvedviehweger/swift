// REQUIRES: objc_interop
// RUN: rm -rf %t && mkdir -p %t && %target-swift-frontend -c -update-code -primary-file %s -F %S/mock-sdk -api-diff-data-file %S/API.json -emit-migrated-file-path %t/property.swift.result -disable-migrator-fixits -o /dev/null
// RUN: diff -u %S/property.swift.expected %t/property.swift.result

import Bar

func foo(_ a : PropertyUserInterface) {
  a.setField(1)
  _ = a.field()
}
