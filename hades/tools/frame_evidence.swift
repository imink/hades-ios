import Foundation
import CoreGraphics
import Vision

enum FrameError: Error {
    case malformedPPM
    case unavailableImage
}

func image(_ path: String) throws -> CGImage {
    let data = try Data(contentsOf: URL(fileURLWithPath: path))
    var ends: [Int] = []
    for index in 0..<min(data.count, 256) where data[index] == 10 {
        ends.append(index)
        if ends.count == 3 { break }
    }
    guard ends.count == 3,
          let header = String(data: data.prefix(ends[2] + 1), encoding: .ascii) else {
        throw FrameError.malformedPPM
    }
    let fields = header.split(whereSeparator: { $0.isWhitespace })
    guard fields.count == 4, fields[0] == "P6", fields[3] == "255",
          let width = Int(fields[1]), let height = Int(fields[2]),
          width > 0, height > 0, width <= 8192, height <= 8192,
          data.count - ends[2] - 1 == width * height * 3 else {
        throw FrameError.malformedPPM
    }
    let pixels = data.suffix(from: ends[2] + 1)
    guard let provider = CGDataProvider(data: Data(pixels) as CFData),
          let result = CGImage(width: width, height: height, bitsPerComponent: 8,
            bitsPerPixel: 24, bytesPerRow: width * 3, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
            provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent) else {
        throw FrameError.unavailableImage
    }
    return result
}

do {
    guard CommandLine.arguments.count >= 2 else { throw FrameError.malformedPPM }
    for path in CommandLine.arguments.dropFirst() {
        let frame = try image(path)
        let request = VNRecognizeTextRequest()
        request.recognitionLevel = .accurate
        request.recognitionLanguages = ["en-US"]
        request.usesLanguageCorrection = false
        try VNImageRequestHandler(cgImage: frame, options: [:]).perform([request])
        let strings = (request.results ?? []).compactMap { $0.topCandidates(1).first }
            .filter { $0.confidence >= 0.1 }.map { $0.string.uppercased() }
        let words = Set(strings.flatMap { $0.split(whereSeparator: { !$0.isLetter }).map(String.init) })
        let markers = ["HADES", "PLAY", "OPTIONS", "CONTINUE", "START", "PRESS", "BACK", "CREDITS",
            "GAMEPLAY", "AUDIO", "DISPLAY", "CONTROLS", "VIDEO", "LANGUAGE", "SELECT", "SAVE", "SLOT", "DELETE", "NEW", "RETURN",
            "ATTACK", "DASH", "CAST", "MOVE", "SPECIAL", "HEALTH", "ESCAPE", "TARTARUS"]
        var detected: [String: Bool] = [:]
        for marker in markers {
            detected[marker] = words.contains(marker)
        }
        let result: [String: Any] = ["file": URL(fileURLWithPath: path).lastPathComponent,
            "width": frame.width, "height": frame.height, "recognized_blocks": strings.count,
            "menu_markers": detected, "processing": "local Apple Vision; no image/text upload"]
        let output = try JSONSerialization.data(withJSONObject: result, options: [.sortedKeys])
        print(String(decoding: output, as: UTF8.self))
    }
} catch {
    FileHandle.standardError.write(Data("Local frame evidence failed: \(error)\n".utf8))
    exit(2)
}
