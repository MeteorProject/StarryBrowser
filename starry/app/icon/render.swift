// 用 WebKit 把 SVG 渲染成透明底的 PNG。
// 用法：render <输入.svg> <输出.png> <边长> [<输入.svg> <输出.png> <边长> …]
import AppKit
import ImageIO
import UniformTypeIdentifiers
import WebKit

struct Job { let input: URL; let output: URL; let size: Int }

@MainActor
final class Renderer: NSObject, WKNavigationDelegate {
    var jobs: [Job]
    var window: NSWindow?
    var web: WKWebView?
    var current: Job?

    init(jobs: [Job]) { self.jobs = jobs }

    func next() {
        guard let job = jobs.first else { NSApp.terminate(nil); return }
        jobs.removeFirst()
        current = job
        guard let svg = try? String(contentsOf: job.input, encoding: .utf8) else {
            print("读不了 \(job.input.path)"); exit(1)
        }
        let frame = NSRect(x: 0, y: 0, width: job.size, height: job.size)
        let window = NSWindow(contentRect: frame.offsetBy(dx: -20000, dy: -20000), styleMask: [.borderless], backing: .buffered, defer: false)
        let web = WKWebView(frame: frame, configuration: WKWebViewConfiguration())
        web.setValue(false, forKey: "drawsBackground")
        web.navigationDelegate = self
        window.contentView = web
        window.orderBack(nil)
        self.window = window
        self.web = web
        let css = "html,body{margin:0;background:transparent}svg{display:block;width:\(job.size)px;height:\(job.size)px}"
        web.loadHTMLString("<!doctype html><style>\(css)</style>\(svg)", baseURL: nil)
    }

    nonisolated func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(200))
            self.snapshot()
        }
    }

    nonisolated func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        print("载入失败：\(error)"); exit(1)
    }

    func snapshot() {
        guard let job = current, let web, let window else { return }
        let config = WKSnapshotConfiguration()
        config.rect = CGRect(x: 0, y: 0, width: job.size, height: job.size)
        // snapshotWidth 的单位是点：除以屏幕倍率，得到的就是一比一的像素，小尺寸不会被二次缩放弄糊。
        config.snapshotWidth = NSNumber(value: Double(job.size) / window.backingScaleFactor)
        web.takeSnapshot(with: config) { image, error in
            guard let image, let cg = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
                print("截图失败：\(String(describing: error))"); exit(1)
            }
            let space = CGColorSpace(name: CGColorSpace.sRGB)!
            let ctx = CGContext(data: nil, width: job.size, height: job.size, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            ctx.interpolationQuality = .high
            ctx.draw(cg, in: CGRect(x: 0, y: 0, width: job.size, height: job.size))
            let dest = CGImageDestinationCreateWithURL(job.output as CFURL, UTType.png.identifier as CFString, 1, nil)!
            CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
            guard CGImageDestinationFinalize(dest) else { print("写不了 \(job.output.path)"); exit(1) }
            print("\(job.output.lastPathComponent) \(job.size)×\(job.size)（快照 \(cg.width)×\(cg.height)）")
            self.window?.orderOut(nil)
            self.next()
        }
    }
}

let args = Array(CommandLine.arguments.dropFirst())
guard !args.isEmpty, args.count % 3 == 0 else { print("用法：render <输入.svg> <输出.png> <边长> …"); exit(2) }
var jobs: [Job] = []
for i in stride(from: 0, to: args.count, by: 3) {
    jobs.append(Job(input: URL(fileURLWithPath: args[i]), output: URL(fileURLWithPath: args[i + 1]), size: Int(args[i + 2])!))
}
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let renderer = MainActor.assumeIsolated { Renderer(jobs: jobs) }
MainActor.assumeIsolated { renderer.next() }
app.run()
