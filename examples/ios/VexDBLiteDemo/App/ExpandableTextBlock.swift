import SwiftUI

struct ExpandableTextBlock: View {
    let title: String
    let text: String
    var trailingText: String? = nil
    var accent = DemoPalette.cyan
    var collapsedLines = 3

    @State private var expanded = false

    var body: some View {
        Button {
            withAnimation(.easeOut(duration: 0.22)) { expanded.toggle() }
        } label: {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 10) {
                    Text(title)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(accent)
                    Spacer()
                    if let trailingText {
                        Text(trailingText)
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.white.opacity(0.45))
                    }
                    Image(systemName: "chevron.down")
                        .font(.caption2.weight(.bold))
                        .foregroundStyle(.white.opacity(0.42))
                        .rotationEffect(.degrees(expanded ? 180 : 0))
                }
                Text(text)
                    .font(.subheadline)
                    .foregroundStyle(.white.opacity(0.76))
                    .multilineTextAlignment(.leading)
                    .lineLimit(expanded ? nil : collapsedLines)
                    .fixedSize(horizontal: false, vertical: true)
                Text(expanded ? "收起" : "展开全文")
                    .font(.caption2.weight(.medium))
                    .foregroundStyle(.white.opacity(0.38))
            }
            .padding(.vertical, 5)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("\(title)，\(expanded ? "点击收起" : "点击展开")")
    }
}

enum DemoPalette {
    static let background = Color(red: 0.035, green: 0.055, blue: 0.105)
    static let panel = Color.white.opacity(0.065)
    static let cyan = Color(red: 0.25, green: 0.91, blue: 0.88)
    static let violet = Color(red: 0.57, green: 0.48, blue: 1.0)
}

extension View {
    func demoCard() -> some View {
        padding(16)
            .background(DemoPalette.panel, in: RoundedRectangle(cornerRadius: 20))
            .overlay {
                RoundedRectangle(cornerRadius: 20)
                    .stroke(Color.white.opacity(0.07), lineWidth: 1)
            }
    }
}
