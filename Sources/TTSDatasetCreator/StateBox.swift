import Combine
import SwiftUI

/// Stand-in for `@State`. In the macOS 27 SDK `@State` is a macro whose compiler plugin (SwiftUIMacros)
/// ships only with Xcode, so with the Command Line Tools alone we keep view-local state in an
/// ObservableObject held by `@StateObject` (same lifetime semantics as `@State`).
final class StateBox<Value>: ObservableObject {
    @Published var value: Value
    init(_ value: Value) { self.value = value }
}
