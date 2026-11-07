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
//   - 旋转 90/270 时裁切框是 4:3 —— 这块区域转到面板上正好变回 3:4；
//   - 打开「允许裁切框超出原图」后，框可以超出原图，最多到“完整包住整图”，
//     超出的部分填白（16:9 旋转 90° 后就能完整显示）；关掉时只能落在画面内。
// 所有坐标归一化到**未旋转**的源图；允许白边时可以在 0…1 之外，和缩略图/原图像素尺寸无关。

enum CropMath {
    static let panelAspect: CGFloat = CGFloat(Spectrum6.width) / CGFloat(Spectrum6.height)
    static let minWidth: CGFloat = 0.04
    /// 允许白边时，裁切框至少要和原图保留的归一化重叠
    static let minOverlap: CGFloat = 0.05

    /// 源图坐标系里裁切框的宽高比（像素宽/高）
    static func frameAspect(rotate: Int) -> CGFloat {
        let degrees = ((rotate % 360) + 360) % 360
        return (degrees == 90 || degrees == 270) ? 1 / panelAspect : panelAspect
    }

    /// 居中、尽可能大的裁切框（不超出原图）
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

    /// 居中、完整包住整图的最小裁切框（可能超出原图 → 转换时填白）
    static func centeredContain(imageSize: CGSize, aspect: CGFloat = panelAspect) -> CGRect {
        guard imageSize.width > 0, imageSize.height > 0, aspect > 0 else {
            return CGRect(x: 0, y: 0, width: 1, height: 1)
        }
        let imageAspect = imageSize.width / imageSize.height
        if imageAspect > aspect {
            // 图更宽：框取满宽，上下留白
            let height = imageAspect / aspect
            return CGRect(x: 0, y: (1 - height) / 2, width: 1, height: height)
        } else {
            // 图更高：框取满高，左右留白
            let width = aspect / imageAspect
            return CGRect(x: (1 - width) / 2, y: 0, width: width, height: 1)
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

    /// 默认裁切框：4:3 时完整包住整图（留白），其余居中最大（裁掉多余）
    static func defaultCrop(imageSize: CGSize, aspect: CGFloat, allowsOverscan: Bool) -> CGRect {
        allowsOverscan
            ? centeredContain(imageSize: imageSize, aspect: aspect)
            : centeredMax(imageSize: imageSize, aspect: aspect)
    }

    /// 把外部传入的裁切框收紧到合法范围
    /// - Parameter allowsOverscan: 打开“允许超出原图”时为 true，框可以超出原图，超出的部分转换时填白。
    static func sanitized(_ rect: CGRect, imageSize: CGSize,
                          aspect: CGFloat = panelAspect,
                          allowsOverscan: Bool = false) -> CGRect {
        guard imageSize.width > 0, imageSize.height > 0 else {
            return CGRect(x: 0, y: 0, width: 1, height: 1)
        }
        if rect.width <= 0.01 || rect.height <= 0.01 {
            return defaultCrop(imageSize: imageSize, aspect: aspect, allowsOverscan: allowsOverscan)
        }
        if !allowsOverscan {
            if rect.width > 1.0001 || rect.height > 1.0001 {
                return centeredMax(imageSize: imageSize, aspect: aspect)
            }
            let x = min(max(rect.minX, 0), 1 - rect.width)
            let y = min(max(rect.minY, 0), 1 - rect.height)
            return CGRect(x: x, y: y, width: rect.width, height: rect.height)
        }
        return limitedOverscan(rect, imageSize: imageSize, aspect: aspect)
    }

    /// 允许白边时的硬限制（不强制比例，给 CLI 的任意裁切框兜底）：
    /// 尺寸不超过“完整包住整图”的框，并且和原图保留一点重叠。
    static func limitedOverscan(_ rect: CGRect, imageSize: CGSize,
                                aspect: CGFloat = panelAspect) -> CGRect {
        guard imageSize.width > 0, imageSize.height > 0, aspect > 0 else { return rect }
        let box = centeredContain(imageSize: imageSize, aspect: aspect)
        let w = min(max(rect.width, 0.01), box.width)
        let h = min(max(rect.height, 0.01), box.height)

        let minX = max(box.minX, minOverlap - w)
        let maxX = min(box.maxX - w, 1 - minOverlap)
        let minY = max(box.minY, minOverlap - h)
        let maxY = min(box.maxY - h, 1 - minOverlap)
        let x = minX <= maxX ? min(max(rect.minX, minX), maxX) : box.midX - w / 2
        let y = minY <= maxY ? min(max(rect.minY, minY), maxY) : box.midY - h / 2
        return CGRect(x: x, y: y, width: w, height: h)
    }

    /// 编辑器用的限制：保持宽高比，框最大只能到“完整包住整图”，且至少留一点重叠。
    static func constrainedOverscan(_ rect: CGRect, imageSize: CGSize,
                                    aspect: CGFloat = panelAspect) -> CGRect {
        let aspectN = normalizedAspect(imageSize: imageSize, aspect: aspect)
        guard aspectN > 0, imageSize.width > 0, imageSize.height > 0 else { return rect }
        let box = centeredContain(imageSize: imageSize, aspect: aspect)
        var w = min(max(rect.width, minWidth), box.width)
        var h = w / aspectN
        if h > box.height {
            h = box.height
            w = h * aspectN
        }

        let minX = max(box.minX, minOverlap - w)
        let maxX = min(box.maxX - w, 1 - minOverlap)
        let minY = max(box.minY, minOverlap - h)
        let maxY = min(box.maxY - h, 1 - minOverlap)
        let x = minX <= maxX ? min(max(rect.minX, minX), maxX) : box.midX - w / 2
        let y = minY <= maxY ? min(max(rect.minY, minY), maxY) : box.midY - h / 2
        return CGRect(x: x, y: y, width: w, height: h)
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
    /// 打开「允许超出原图」时裁切框可以超出原图，空白处填白
    let allowsOverscan: Bool
    @Binding var crop: CGRect
    let enabled: Bool
    let onCommit: () -> Void

    @State private var dragStart: CGRect?

    private var aspect: CGFloat {
        CropMath.normalizedAspect(imageSize: imageSize, aspect: aspectRatio)
    }

    /// 归一化坐标（相对未旋转的源图）→ 视图坐标。
    /// 允许白边时 extent 是“完整包住整图”的框，会比原图大，超出的地方就是白边。
    private struct Canvas {
        let extent: CGRect
        let scaleX: CGFloat
        let scaleY: CGFloat
        let origin: CGPoint

        func rect(_ r: CGRect) -> CGRect {
            CGRect(x: origin.x + (r.minX - extent.minX) * scaleX,
                   y: origin.y + (r.minY - extent.minY) * scaleY,
                   width: r.width * scaleX,
                   height: r.height * scaleY)
        }

        var imageRect: CGRect { rect(CGRect(x: 0, y: 0, width: 1, height: 1)) }
        var extentRect: CGRect { rect(extent) }
    }

    var body: some View {
        GeometryReader { geo in
            if let canvas = makeCanvas(in: geo.size) {
                ZStack {
                    Color(nsColor: .underPageBackgroundColor)

                    /* 白边预览：先铺一张白纸，图片盖在上面，露出来的就是会生成的白色 */
                    if allowsOverscan && enabled {
                        Rectangle()
                            .fill(Color.white)
                            .frame(width: canvas.extentRect.width,
                                   height: canvas.extentRect.height)
                            .position(x: canvas.extentRect.midX,
                                      y: canvas.extentRect.midY)
                    }

                    Image(decorative: image, scale: 1)
                        .resizable()
                        .interpolation(.high)
                        .frame(width: canvas.imageRect.width,
                               height: canvas.imageRect.height)
                        .position(x: canvas.imageRect.midX, y: canvas.imageRect.midY)
                        .shadow(radius: 10, y: 3)

                    /* 原图边界：白边和原图才分得清 */
                    if allowsOverscan && enabled {
                        Path { $0.addRect(canvas.imageRect) }
                            .stroke(Color.black.opacity(0.22),
                                    style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
                            .allowsHitTesting(false)
                    }

                    if enabled {
                        let rect = canvas.rect(crop)
                        overlay(rect: rect, container: geo.size)
                        moveArea(canvas: canvas, rect: rect)
                        handles(canvas: canvas, rect: rect)
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
            } else {
                Color(nsColor: .underPageBackgroundColor)
            }
        }
    }

    // MARK: 布局

    private func makeCanvas(in container: CGSize) -> Canvas? {
        guard imageSize.width > 0, imageSize.height > 0,
              container.width > 16, container.height > 16 else {
            return nil
        }

        /* 打开白边时把“完整包住整图”的框也放进可视区域，四角手柄才拖得到 */
        let extent = (allowsOverscan && enabled)
            ? CropMath.centeredContain(imageSize: imageSize, aspect: aspectRatio)
            : CGRect(x: 0, y: 0, width: 1, height: 1)

        let pixelWidth = extent.width * imageSize.width
        let pixelHeight = extent.height * imageSize.height
        guard pixelWidth > 0, pixelHeight > 0 else { return nil }

        /* 留一圈边距，四角手柄不会被裁掉 */
        let available = CGSize(width: container.width - 36, height: container.height - 36)
        guard available.width > 0, available.height > 0 else { return nil }
        let scale = min(available.width / pixelWidth, available.height / pixelHeight)
        let scaleX = scale * imageSize.width
        let scaleY = scale * imageSize.height

        return Canvas(extent: extent,
                      scaleX: scaleX,
                      scaleY: scaleY,
                      origin: CGPoint(x: (container.width - extent.width * scaleX) / 2,
                                      y: (container.height - extent.height * scaleY) / 2))
    }

    // MARK: 遮罩 / 网格 / 边框

    private func overlay(rect: CGRect, container: CGSize) -> some View {
        ZStack {
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

            /* 边框：白线在白色留白上也要看得见，先垫一圈暗边 */
            Rectangle()
                .path(in: rect)
                .stroke(Color.black.opacity(0.4), lineWidth: 3.5)
            Rectangle()
                .path(in: rect)
                .stroke(Color.white, lineWidth: 2)
        }
        .allowsHitTesting(false)
    }

    // MARK: 拖动整框

    private func moveArea(canvas: Canvas, rect: CGRect) -> some View {
        Rectangle()
            .fill(Color.white.opacity(0.001))
            .frame(width: rect.width, height: rect.height)
            .position(x: rect.midX, y: rect.midY)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 1)
                    .onChanged { value in
                        if dragStart == nil { dragStart = crop }
                        guard let start = dragStart else { return }

                        let dx = value.translation.width / canvas.scaleX
                        let dy = value.translation.height / canvas.scaleY
                        var next = CGRect(x: start.minX + dx,
                                          y: start.minY + dy,
                                          width: start.width,
                                          height: start.height)

                        if allowsOverscan {
                            next = CropMath.constrainedOverscan(next,
                                                                imageSize: imageSize,
                                                                aspect: aspectRatio)
                        } else {
                            next.origin = CGPoint(
                                x: min(max(next.minX, 0), max(0, 1 - next.width)),
                                y: min(max(next.minY, 0), max(0, 1 - next.height)))
                        }
                        crop = next
                    }
                    .onEnded { _ in
                        dragStart = nil
                        onCommit()
                    }
            )
    }

    // MARK: 四角缩放

    private func handles(canvas: Canvas, rect: CGRect) -> some View {
        ForEach(Corner.allCases, id: \.self) { corner in
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
            .gesture(resizeGesture(corner: corner, canvas: canvas))
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

    private func resizeGesture(corner: Corner, canvas: Canvas) -> some Gesture {
        DragGesture(minimumDistance: 1)
            .onChanged { value in
                if dragStart == nil { dragStart = crop }
                guard let start = dragStart else { return }

                let dx = value.translation.width / canvas.scaleX
                let dy = value.translation.height / canvas.scaleY

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

                let maxWidth: CGFloat
                if allowsOverscan {
                    /* 打开白边时可以长到“完整包住整图”的框，但不能再大 */
                    let box = CropMath.centeredContain(imageSize: imageSize, aspect: aspectRatio)
                    let maxWidthByX = isLeft ? anchorX - box.minX : box.maxX - anchorX
                    let maxWidthByY = (isTop ? anchorY - box.minY : box.maxY - anchorY) * aspect
                    maxWidth = min(box.width, maxWidthByX, maxWidthByY)
                } else {
                    let maxWidthByX = isLeft ? anchorX : 1 - anchorX
                    let maxHeightByY = isTop ? anchorY : 1 - anchorY
                    maxWidth = min(maxWidthByX, maxHeightByY * aspect)
                }

                width = min(max(width, CropMath.minWidth), max(maxWidth, 0.01))
                height = width / aspect

                let x = isLeft ? anchorX - width : anchorX
                let y = isTop ? anchorY - height : anchorY

                var rect = CGRect(x: x, y: y, width: width, height: height)
                if allowsOverscan {
                    rect = CropMath.constrainedOverscan(rect,
                                                        imageSize: imageSize,
                                                        aspect: aspectRatio)
                }
                crop = rect
            }
            .onEnded { _ in
                dragStart = nil
                onCommit()
            }
    }
}
