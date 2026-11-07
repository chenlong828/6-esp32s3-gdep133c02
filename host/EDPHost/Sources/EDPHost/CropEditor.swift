import SwiftUI

// MARK: - 视图模式

enum PreviewMode: String, CaseIterable, Identifiable {
    case crop
    case result

    var id: String { rawValue }

    var label: String {
        self == .crop ? "裁切" : "成品"
    }
}

// MARK: - 裁切几何
//
// 裁切顺序是「先裁切、后旋转」：
//   - 编辑器里始终显示原始方向的图（手机竖拍也不会歪着看）；
//   - 旋转 0/180 时裁切框是面板的 3:4；
//   - 旋转 90/270 时裁切框是 4:3 —— 这块区域转到面板上正好变回 3:4。
// 所有坐标归一化到**未旋转**的源图（0…1），和缩略图/原图像素尺寸无关。

enum CropMath {
    static let panelAspect: CGFloat = CGFloat(Spectrum6.width) / CGFloat(Spectrum6.height)
    static let minWidth: CGFloat = 0.04

    /// 源图坐标系里裁切框的宽高比（像素宽/高）
    static func frameAspect(rotate: Int) -> CGFloat {
        let degrees = ((rotate % 360) + 360) % 360
        return (degrees == 90 || degrees == 270) ? 1 / panelAspect : panelAspect
    }

    /// 居中、尽可能大的裁切框
    static func centeredMax(imageSize: CGSize, aspect: CGFloat = panelAspect) -> CGRect {
        guard imageSize.width > 0, imageSize.height > 0, aspect > 0 else {
            return CGRect(x: 0, y: 0, width: 1, height: 1)
        }
        let imageAspect = imageSize.width / imageSize.height
        if imageAspect > aspect {
            // 图更宽：左右裁
            let width = aspect / imageAspect
            return CGRect(x: (1 - width) / 2, y: 0, width: width, height: 1)
        } else {
            // 图更高：上下裁
            let height = imageAspect / aspect
            return CGRect(x: 0, y: (1 - height) / 2, width: 1, height: height)
        }
    }

    /// 归一化坐标下的宽高比（宽 / 高）
    static func normalizedAspect(imageSize: CGSize, aspect: CGFloat) -> CGFloat {
        guard imageSize.width > 0 else { return aspect }
        return aspect * imageSize.height / imageSize.width
    }

    static func pixelSize(_ rect: CGRect, imageSize: CGSize) -> CGSize {
        CGSize(width: rect.width * imageSize.width,
               height: rect.height * imageSize.height)
    }

    /// 把外部传入的裁切框收紧到合法范围
    static func sanitized(_ rect: CGRect, imageSize: CGSize,
                          aspect: CGFloat = panelAspect) -> CGRect {
        if rect.width <= 0.01 || rect.height <= 0.01
            || rect.width > 1.0001 || rect.height > 1.0001 {
            return centeredMax(imageSize: imageSize, aspect: aspect)
        }
        let x = min(max(rect.minX, 0), 1 - rect.width)
        let y = min(max(rect.minY, 0), 1 - rect.height)
        return CGRect(x: x, y: y, width: rect.width, height: rect.height)
    }

    /// 把“未旋转源图”的裁切框映射到“旋转后”的坐标系（归一化，y 向下）
    static func rotated(_ rect: CGRect, rotate: Int) -> CGRect {
        switch ((rotate % 360) + 360) % 360 {
        case 90:
            return CGRect(x: 1 - rect.maxY, y: rect.minX,
                          width: rect.height, height: rect.width)
        case 180:
            return CGRect(x: 1 - rect.maxX, y: 1 - rect.maxY,
                          width: rect.width, height: rect.height)
        case 270:
            return CGRect(x: rect.minY, y: 1 - rect.maxX,
                          width: rect.height, height: rect.width)
        default:
            return rect
        }
    }
}

// MARK: - 裁切编辑器

struct CropEditorView: View {
    enum Corner: CaseIterable {
        case topLeft, topRight, bottomLeft, bottomRight
    }

    let image: CGImage
    let imageSize: CGSize
    /// 裁切框的像素宽高比（3:4 或 4:3，跟随旋转）
    let aspectRatio: CGFloat
    @Binding var crop: CGRect
    let enabled: Bool
    let onCommit: () -> Void

    @State private var dragStart: CGRect?

    private var aspect: CGFloat {
        CropMath.normalizedAspect(imageSize: imageSize, aspect: aspectRatio)
    }

    var body: some View {
        GeometryReader { geo in
            let layout = fittedRect(in: geo.size)

            ZStack {
                Color(nsColor: .underPageBackgroundColor)

                Image(decorative: image, scale: 1)
                    .resizable()
                    .interpolation(.high)
                    .frame(width: layout.width, height: layout.height)
                    .position(x: layout.midX, y: layout.midY)
                    .shadow(radius: 10, y: 3)

                if enabled {
                    overlay(layout: layout, container: geo.size)
                    moveArea(layout: layout)
                    handles(layout: layout)
                } else {
                    VStack {
                        Spacer()
                        Text("整图模式（不裁切）")
                            .font(.caption)
                            .padding(.horizontal, 10)
                            .padding(.vertical, 5)
                            .background(.regularMaterial, in: Capsule())
                            .padding(.bottom, 14)
                    }
                }
            }
            .clipped()
        }
    }

    // MARK: 布局

    private func fittedRect(in container: CGSize) -> CGRect {
        guard imageSize.width > 0, imageSize.height > 0,
              container.width > 16, container.height > 16 else {
            return .zero
        }
        /* 留一圈边距，四角手柄不会被裁掉 */
        let available = CGSize(width: container.width - 28, height: container.height - 28)
        let scale = min(available.width / imageSize.width,
                        available.height / imageSize.height)
        let size = CGSize(width: imageSize.width * scale,
                          height: imageSize.height * scale)
        return CGRect(x: (container.width - size.width) / 2,
                      y: (container.height - size.height) / 2,
                      width: size.width,
                      height: size.height)
    }

    private func viewRect(_ layout: CGRect) -> CGRect {
        CGRect(x: layout.minX + crop.minX * layout.width,
               y: layout.minY + crop.minY * layout.height,
               width: crop.width * layout.width,
               height: crop.height * layout.height)
    }

    // MARK: 遮罩 / 网格 / 边框

    private func overlay(layout: CGRect, container: CGSize) -> some View {
        let rect = viewRect(layout)

        return ZStack {
            /* 框外压暗 */
            Path { path in
                path.addRect(CGRect(origin: .zero, size: container))
                path.addRect(rect)
            }
            .fill(Color.black.opacity(0.55), style: FillStyle(eoFill: true))

            /* 三分线 */
            Path { path in
                for i in 1...2 {
                    let x = rect.minX + rect.width * CGFloat(i) / 3
                    path.move(to: CGPoint(x: x, y: rect.minY))
                    path.addLine(to: CGPoint(x: x, y: rect.maxY))

                    let y = rect.minY + rect.height * CGFloat(i) / 3
                    path.move(to: CGPoint(x: rect.minX, y: y))
                    path.addLine(to: CGPoint(x: rect.maxX, y: y))
                }
            }
            .stroke(Color.white.opacity(0.45), lineWidth: 0.5)

            /* 边框 */
            Rectangle()
                .path(in: rect)
                .stroke(Color.white, lineWidth: 2)
        }
        .allowsHitTesting(false)
    }

    // MARK: 拖动整框

    private func moveArea(layout: CGRect) -> some View {
        let rect = viewRect(layout)

        return Rectangle()
            .fill(Color.white.opacity(0.001))
            .frame(width: rect.width, height: rect.height)
            .position(x: rect.midX, y: rect.midY)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 1)
                    .onChanged { value in
                        if dragStart == nil { dragStart = crop }
                        guard let start = dragStart, layout.width > 0, layout.height > 0 else {
                            return
                        }
                        let dx = value.translation.width / layout.width
                        let dy = value.translation.height / layout.height
                        let x = min(max(start.minX + dx, 0), max(0, 1 - start.width))
                        let y = min(max(start.minY + dy, 0), max(0, 1 - start.height))
                        crop = CGRect(x: x, y: y, width: start.width, height: start.height)
                    }
                    .onEnded { _ in
                        dragStart = nil
                        onCommit()
                    }
            )
    }

    // MARK: 四角缩放

    private func handles(layout: CGRect) -> some View {
        let rect = viewRect(layout)

        return ForEach(Corner.allCases, id: \.self) { corner in
            let point = cornerPoint(corner, in: rect)

            ZStack {
                Circle()
                    .fill(Color.white)
                    .frame(width: 11, height: 11)
                    .shadow(radius: 2)
                Circle()
                    .strokeBorder(Color.black.opacity(0.35), lineWidth: 1)
                    .frame(width: 11, height: 11)
            }
            .frame(width: 34, height: 34)
            .contentShape(Rectangle())
            .position(point)
            .gesture(resizeGesture(corner: corner, layout: layout))
        }
    }

    private func cornerPoint(_ corner: Corner, in rect: CGRect) -> CGPoint {
        switch corner {
        case .topLeft:     return CGPoint(x: rect.minX, y: rect.minY)
        case .topRight:    return CGPoint(x: rect.maxX, y: rect.minY)
        case .bottomLeft:  return CGPoint(x: rect.minX, y: rect.maxY)
        case .bottomRight: return CGPoint(x: rect.maxX, y: rect.maxY)
        }
    }

    private func resizeGesture(corner: Corner, layout: CGRect) -> some Gesture {
        DragGesture(minimumDistance: 1)
            .onChanged { value in
                if dragStart == nil { dragStart = crop }
                guard let start = dragStart, layout.width > 0, layout.height > 0 else {
                    return
                }

                let dx = value.translation.width / layout.width
                let dy = value.translation.height / layout.height

                let isLeft = (corner == .topLeft || corner == .bottomLeft)
                let isTop = (corner == .topLeft || corner == .topRight)

                /* 对角是固定锚点 */
                let anchorX = isLeft ? start.maxX : start.minX
                let anchorY = isTop ? start.maxY : start.minY

                /* 被拖动那个角的位置 */
                let targetX = (isLeft ? start.minX : start.maxX) + dx
                let targetY = (isTop ? start.minY : start.maxY) + dy

                var width = abs(targetX - anchorX)
                var height = abs(targetY - anchorY)

                /* 先按比例对齐，再受锚点可用空间限制 */
                if height < 1e-6 || width / height > aspect {
                    height = width / aspect
                } else {
                    width = height * aspect
                }

                let maxWidthByX = isLeft ? anchorX : 1 - anchorX
                let maxHeightByY = isTop ? anchorY : 1 - anchorY
                let maxWidth = max(min(maxWidthByX, maxHeightByY * aspect), 0.01)

                width = min(max(width, 0.01), maxWidth)
                height = width / aspect

                let x = isLeft ? anchorX - width : anchorX
                let y = isTop ? anchorY - height : anchorY

                crop = CGRect(x: x, y: y, width: width, height: height)
            }
            .onEnded { _ in
                dragStart = nil
                onCommit()
            }
    }
}
