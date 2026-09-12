import Foundation

/// HTTP client for transferring save data between iOS and pkwbridge on 3DS.
/// Protocol matches PKSM's WirelessTransfer: simple HTTP REST on port 8000.
///
/// Endpoints:
///   GET  /transfer/info    → JSON { device, version, maxUploadBytes }
///   POST /transfer/upload  → multipart/form-data with EEPROM data
///   GET  /transfer/download → raw EEPROM file (65536 bytes)
class NetworkTransferClient {

    struct ConnectionInfo: Decodable {
        let device: String
        let version: String
        let maxUploadBytes: Int?
    }

    struct TransferResponse: Decodable {
        let ok: Bool
        let error: String?
        let savedPath: String?
    }

    enum TransferError: LocalizedError {
        case connectionFailed(String)
        case invalidResponse(Int)
        case dataCorrupted(String)
        case serverError(String)

        var errorDescription: String? {
            switch self {
            case .connectionFailed(let msg): return "Connection failed: \(msg)"
            case .invalidResponse(let code): return "Invalid server response: HTTP \(code)"
            case .dataCorrupted(let msg): return "Data corrupted: \(msg)"
            case .serverError(let msg): return "Server error: \(msg)"
            }
        }
    }

    // MARK: - Connection Info

    static func getInfo(host: String, port: Int) async throws -> ConnectionInfo {
        guard let url = URL(string: "http://\(host):\(port)/transfer/info") else {
            throw TransferError.connectionFailed("Invalid URL")
        }

        let (data, response) = try await URLSession.shared.data(from: url)

        guard let httpResponse = response as? HTTPURLResponse,
              httpResponse.statusCode == 200 else {
            let code = (response as? HTTPURLResponse)?.statusCode ?? -1
            throw TransferError.invalidResponse(code)
        }

        let decoder = JSONDecoder()
        return try decoder.decode(ConnectionInfo.self, from: data)
    }

    // MARK: - Upload EEPROM (iOS → 3DS)

    static func uploadEEPROM(host: String, port: Int, eepromData: Data) async throws -> Bool {
        guard let url = URL(string: "http://\(host):\(port)/transfer/upload") else {
            throw TransferError.connectionFailed("Invalid URL")
        }

        let boundary = "----pokestride-boundary-\(UUID().uuidString)"

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("multipart/form-data; boundary=\(boundary)", forHTTPHeaderField: "Content-Type")
        request.setValue("close", forHTTPHeaderField: "Connection")
        request.timeoutInterval = 30

        // Build multipart body
        var body = Data()

        // Metadata part
        let metadata: [String: Any] = [
            "titleId": "PokeStride-iOS",
            "titleName": "PokeStride iOS",
            "dataType": "eeprom",
            "backupName": "pweep.rom",
            "isZip": false,
            "fileBytesTotal": eepromData.count,
            "fileName": "pweep.rom"
        ]

        let metadataJSON = try JSONSerialization.data(withJSONObject: metadata)
        let metadataString = String(data: metadataJSON, encoding: .utf8) ?? "{}"

        body.append(Data("--\(boundary)\r\n".utf8))
        body.append(Data("Content-Disposition: form-data; name=\"meta\"\r\n".utf8))
        body.append(Data("Content-Type: application/json\r\n\r\n".utf8))
        body.append(Data(metadataString.utf8))
        body.append(Data("\r\n".utf8))

        // File part
        body.append(Data("--\(boundary)\r\n".utf8))
        body.append(Data("Content-Disposition: form-data; name=\"file\"; filename=\"pweep.rom\"\r\n".utf8))
        body.append(Data("Content-Type: application/octet-stream\r\n\r\n".utf8))
        body.append(eepromData)
        body.append(Data("\r\n".utf8))

        // End boundary
        body.append(Data("--\(boundary)--\r\n".utf8))

        request.httpBody = body
        request.setValue("\(body.count)", forHTTPHeaderField: "Content-Length")

        let (responseData, response) = try await URLSession.shared.data(for: request)

        guard let httpResponse = response as? HTTPURLResponse else {
            throw TransferError.invalidResponse(-1)
        }

        if httpResponse.statusCode == 200 {
            let result = try JSONDecoder().decode(TransferResponse.self, from: responseData)
            return result.ok
        } else {
            throw TransferError.invalidResponse(httpResponse.statusCode)
        }
    }

    // MARK: - Download EEPROM (3DS → iOS)

    static func downloadEEPROM(host: String, port: Int) async throws -> Data {
        guard let url = URL(string: "http://\(host):\(port)/transfer/download") else {
            throw TransferError.connectionFailed("Invalid URL")
        }

        let (data, response) = try await URLSession.shared.data(from: url)

        guard let httpResponse = response as? HTTPURLResponse else {
            throw TransferError.invalidResponse(-1)
        }

        guard httpResponse.statusCode == 200 else {
            throw TransferError.invalidResponse(httpResponse.statusCode)
        }

        guard data.count >= 65536 else {
            throw TransferError.dataCorrupted("EEPROM too small: \(data.count) bytes (expected 65536)")
        }

        return data.prefix(65536)
    }

    // MARK: - Multipeer-style Discovery

    /// Try to discover pkwbridge on the local network using Bonjour/mDNS
    /// Falls back to manual IP entry if not found
    static func discoverPKWBridge() async -> String? {
        // In a real implementation, this would use NWBrowser for Bonjour discovery
        // For now, return nil to require manual IP entry
        return nil
    }
}
