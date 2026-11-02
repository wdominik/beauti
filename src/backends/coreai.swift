// SPDX-License-Identifier: GPL-3.0-or-later
// Core AI adapter: loads one model function and runs it synchronously for the C++
// worker thread, behind the C interface declared in coreai.h.
import CoreAI
import Foundation

// Carries a result out of a detached task to the blocked C caller.
private final class Completion<T>: @unchecked Sendable {
	let done = DispatchSemaphore(value: 0)
	var value: T?
	var error: String?

	func wait(_ body: @escaping @Sendable () async throws -> T) -> Result<T, AdapterError> {
		Task.detached {
			defer { self.done.signal() }
			do { self.value = try await body() } catch { self.error = String(describing: error) }
		}
		done.wait()
		if let value { return .success(value) }
		return .failure(AdapterError(error ?? "unknown Core AI error"))
	}
}

private struct AdapterError: Error {
	let message: String
	init(_ message: String) { self.message = message }
}

// One loaded function. The input tensor is allocated once and refilled per call;
// the C++ engine never runs the same model from two threads at once.
private final class Model: @unchecked Sendable {
	let model: AIModel
	let function: InferenceFunction
	let inputName: String, outputName: String
	let side: Int, channels: Int
	let half: Bool
	var input: NDArray

	init(_ model: AIModel, _ function: InferenceFunction, _ inputName: String, _ outputName: String,
	     _ side: Int, _ channels: Int, _ half: Bool) {
		self.model = model
		self.function = function
		self.inputName = inputName
		self.outputName = outputName
		self.side = side
		self.channels = channels
		self.half = half
		input = NDArray(shape: [1, 3, side, side], scalarType: half ? .float16 : .float32)
	}

	func copy(_ array: NDArray, to target: UnsafeMutablePointer<Float>) throws {
		let count = channels * side * side
		guard array.shape.reduce(1, *) == count else {
			throw AdapterError("unexpected output shape \(array.shape)")
		}
		if half {
			guard let values = array.view(as: Float16.self).contiguousElements else {
				throw AdapterError("noncontiguous output")
			}
			for i in 0..<count { target[i] = Float(values[i]) }
		} else {
			guard let values = array.view(as: Float.self).contiguousElements else {
				throw AdapterError("noncontiguous output")
			}
			for i in 0..<count { target[i] = values[i] }
		}
	}
}

private func report(_ message: String, _ error: UnsafeMutablePointer<CChar>?, _ size: Int) {
	guard let error, size > 0 else { return }
	let bytes = Array(message.utf8.prefix(size - 1))
	for (i, byte) in bytes.enumerated() { error[i] = CChar(bitPattern: byte) }
	error[bytes.count] = 0
}

// The cache folders of this process: ~/Library/Caches/coreai-cache/<macOS build>/<bundle
// identifier or executable name>. Returns whether any folder was removed.
private func clearCoreAICache() -> Bool {
	let files = FileManager.default
	guard let caches = files.urls(for: .cachesDirectory, in: .userDomainMask).first else { return false }
	let root = caches.appendingPathComponent("coreai-cache", isDirectory: true)
	let name = Bundle.main.bundleIdentifier ?? ProcessInfo.processInfo.processName
	let builds = (try? files.contentsOfDirectory(at: root, includingPropertiesForKeys: nil)) ?? []
	var removed = false
	for build in builds {
		let folder = build.appendingPathComponent(name, isDirectory: true)
		if files.fileExists(atPath: folder.path), (try? files.removeItem(at: folder)) != nil {
			removed = true
		}
	}
	return removed
}

@_cdecl("beauti_coreai_load")
public func beautiCoreAILoad(_ asset: UnsafePointer<CChar>, _ input: UnsafePointer<CChar>,
			     _ output: UnsafePointer<CChar>, _ side: Int32, _ channels: Int32, _ half: Int32,
			     _ unit: Int32, _ error: UnsafeMutablePointer<CChar>?, _ errorSize: Int) -> UnsafeMutableRawPointer? {
	let url = URL(fileURLWithPath: String(cString: asset))
	let names = (String(cString: input), String(cString: output))
	// Explicit placement: the automatic choice fails to load these models on the Neural Engine.
	let kind: ComputeUnitKind = unit == 1 ? .gpu : .neuralEngine
	let load = {
		Completion<Model>().wait {
			let options = SpecializationOptions(preferredComputeUnitKind: kind)
			let model = try await AIModel(contentsOf: url, options: options)
			guard let function = try model.loadFunction(named: "main") else { throw AdapterError("no main function") }
			return Model(model, function, names.0, names.1, Int(side), Int(channels), half != 0)
		}
	}
	var result = load()
	// Core AI keeps compiled models per process and fails with nilError instead of recompiling
	// once the process's code signature has changed (a rebuilt or updated host). Clearing this
	// process's cache folder and loading once more recovers; Core AI then rebuilds the cache.
	// Other failures keep the cache: it may serve other models of this process.
	if case .failure(let failure) = result, failure.message.contains("nilError"), clearCoreAICache() {
		result = load()
	}
	switch result {
	case .success(let model): return Unmanaged.passRetained(model).toOpaque()
	case .failure(let failure):
		report("Core AI could not load \(url.lastPathComponent): \(failure.message)", error, errorSize)
		return nil
	}
}

@_cdecl("beauti_coreai_run")
public func beautiCoreAIRun(_ handle: UnsafeMutableRawPointer, _ input: UnsafePointer<Float>,
			    _ output: UnsafeMutablePointer<Float>, _ error: UnsafeMutablePointer<CChar>?,
			    _ errorSize: Int) -> Int32 {
	let model = Unmanaged<Model>.fromOpaque(handle).takeUnretainedValue()
	nonisolated(unsafe) let source = input
	nonisolated(unsafe) let target = output
	let result = Completion<Bool>().wait {
		let count = 3 * model.side * model.side
		if model.half {
			model.input.mutableView(as: Float16.self).withUnsafeMutablePointer { tensor, _, _ in
				for i in 0..<count { tensor[i] = Float16(source[i]) }
			}
		} else {
			model.input.mutableView(as: Float.self).withUnsafeMutablePointer { tensor, _, _ in
				tensor.update(from: source, count: count)
			}
		}
		var values = try await model.function.run(inputs: [model.inputName: model.input])
		guard let value = values.remove(model.outputName), let array = value.ndArray else {
			throw AdapterError("missing output \(model.outputName)")
		}
		try model.copy(array, to: target)
		return true
	}
	if case .failure(let failure) = result {
		report("Core AI inference failed: \(failure.message)", error, errorSize)
		return 1
	}
	return 0
}

@_cdecl("beauti_coreai_release")
public func beautiCoreAIRelease(_ handle: UnsafeMutableRawPointer) {
	Unmanaged<Model>.fromOpaque(handle).release()
}
