// Program color grade — the macOS twin of the WinUI ColorGradeEditorWindow.
//
// The media core has always accepted `set-color-grade` (exposure / contrast /
// saturation / temperature, each clamped core-side) and the Metal shader
// already consumes those constants per composited layer. Nothing on macOS ever
// sent the command, so a Mac operator had NO picture control at all while the
// Windows shell shipped a whole editor window. This is shell-only work.
//
// Grading is a live-eyes task: the operator rides a slider while watching the
// program, so every change pushes immediately rather than behind an Apply.

import SwiftUI

struct ColorGradePane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Text("COLOR GRADE").font(.grotesk(12, .semibold))
                // Honest state: dim when the chain is arithmetically a no-op,
                // the same rule the mastering rack uses for its stages.
                Text(model.gradeIsNeutral ? "neutral" : "active")
                    .font(.plexMono(10))
                    .foregroundStyle(model.gradeIsNeutral ? Studio.textDim : Studio.accent)
                Spacer()
                Button("Reset") { model.resetColorGrade() }
                    .buttonStyle(GhostButtonStyle())
                    .disabled(model.gradeIsNeutral)
            }

            axis("Exposure", value: $model.gradeExposure)
            axis("Contrast", value: $model.gradeContrast)
            axis("Saturation", value: $model.gradeSaturation)
            axis("Temperature", value: $model.gradeTemperature)

            Text("Applies to the composited program — recording, stream and the "
                 + "multiview all inherit it.")
                .font(.grotesk(11)).foregroundStyle(Studio.textDim)
            Spacer(minLength: 0)
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // The core clamps each axis to [-10, 10] (clampColorGradeAxis) and the
    // shader scales by 0.1, so the UI range must match or the operator drags
    // into a region that silently does nothing.
    func axis(_ label: String, value: Binding<Double>) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                Spacer()
                Text(String(format: "%+.1f", value.wrappedValue))
                    .font(.plexMono(11))
                    .foregroundStyle(value.wrappedValue == 0 ? Studio.textDim : Studio.textPrimary)
            }
            // applyColorGrade coalesces, so a drag cannot flood the RPC queue.
            Slider(value: value, in: -10...10, step: 0.1)
                .onChange(of: value.wrappedValue) { _ in model.applyColorGrade() }
        }
    }
}
