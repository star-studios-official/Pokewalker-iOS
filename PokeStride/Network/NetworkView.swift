import SwiftUI

/// Network transfer view — connects to pkwbridge on 3DS via WiFi
/// Protocol mirrors PKSM's WirelessTransfer: HTTP server on 3DS, iOS connects as client
struct NetworkView: View {
    @EnvironmentObject var emulator: PokeWalkerEmulator
    @Environment(\.dismiss) private var dismiss

    @State private var ipAddress = ""
    @State private var port = "8000"
    @State private var isConnected = false
    @State private var isTransferring = false
    @State private var statusMessage = "Ready to connect"
    @State private var showError = false
    @State private var errorMessage = ""
    @State private var connectionInfo: ConnectionInfo?

    struct ConnectionInfo {
        let device: String
        let version: String
    }

    var body: some View {
        NavigationView {
            ZStack {
                // Background
                LinearGradient(
                    gradient: Gradient(colors: [
                        Color(red: 0.05, green: 0.08, blue: 0.15),
                        Color(red: 0.02, green: 0.04, blue: 0.08)
                    ]),
                    startPoint: .top,
                    endPoint: .bottom
                )
                .ignoresSafeArea()

                VStack(spacing: 24) {
                    // Connection Status
                    connectionStatusCard

                    // Connection Form
                    connectionForm

                    // Transfer Controls
                    if isConnected {
                        transferControls
                    }

                    Spacer()

                    // Protocol Info
                    protocolInfo
                }
                .padding(.horizontal, 20)
                .padding(.top, 10)
            }
            .navigationTitle("PKWBridge Transfer")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Done") {
                        disconnect()
                        dismiss()
                    }
                    .font(.system(.body, design: .rounded, weight: .medium))
                }
            }
            .alert("Error", isPresented: $showError) {
                Button("OK") { }
            } message: {
                Text(errorMessage)
            }
        }
    }

    // MARK: - Connection Status Card

    private var connectionStatusCard: some View {
        VStack(spacing: 12) {
            HStack {
                Circle()
                    .fill(isConnected ? .green : .orange)
                    .frame(width: 12, height: 12)
                    .shadow(color: isConnected ? .green : .orange, radius: 4)

                Text(isConnected ? "Connected" : "Disconnected")
                    .font(.system(.body, design: .rounded, weight: .medium))
                    .foregroundStyle(.white)

                Spacer()

                if let info = connectionInfo {
                    VStack(alignment: .trailing) {
                        Text(info.device)
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(.white.opacity(0.6))
                        Text(info.version)
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.white.opacity(0.4))
                    }
                }
            }

            Text(statusMessage)
                .font(.system(.caption, design: .rounded))
                .foregroundStyle(.white.opacity(0.5))
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(16)
        .glassEffect(.regular, in: .rect(cornerRadius: 16))
    }

    // MARK: - Connection Form

    private var connectionForm: some View {
        VStack(spacing: 16) {
            // IP Address
            VStack(alignment: .leading, spacing: 6) {
                Label("3DS IP Address", systemImage: "network")
                    .font(.system(.caption, design: .rounded, weight: .medium))
                    .foregroundStyle(.white.opacity(0.6))

                TextField("192.168.1.x", text: $ipAddress)
                    .font(.system(.body, design: .monospaced))
                    .foregroundStyle(.white)
                    .padding(12)
                    .background(.ultraThinMaterial)
                    .cornerRadius(10)
                    .keyboardType(.numbersAndPunctuation)
                    .autocorrectionDisabled()
            }

            // Port
            VStack(alignment: .leading, spacing: 6) {
                Label("Port", systemImage: "number")
                    .font(.system(.caption, design: .rounded, weight: .medium))
                    .foregroundStyle(.white.opacity(0.6))

                TextField("8000", text: $port)
                    .font(.system(.body, design: .monospaced))
                    .foregroundStyle(.white)
                    .padding(12)
                    .background(.ultraThinMaterial)
                    .cornerRadius(10)
                    .keyboardType(.numberPad)
            }

            // Connect Button
            Button {
                if isConnected {
                    disconnect()
                } else {
                    connect()
                }
            } label: {
                HStack {
                    Image(systemName: isConnected ? "link.circle.slash" : "link.circle.fill")
                    Text(isConnected ? "Disconnect" : "Connect")
                        .font(.system(.body, design: .rounded, weight: .semibold))
                }
                .foregroundStyle(.white)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 14)
                .glassEffect(.regular, in: .capsule)
            }
            .buttonStyle(.plain)
        }
        .padding(16)
        .glassEffect(.regular, in: .rect(cornerRadius: 16))
    }

    // MARK: - Transfer Controls

    private var transferControls: some View {
        VStack(spacing: 12) {
            Text("Transfer Options")
                .font(.system(.caption, design: .rounded, weight: .medium))
                .foregroundStyle(.white.opacity(0.5))
                .frame(maxWidth: .infinity, alignment: .leading)

            // Send EEPROM to 3DS
            Button {
                sendEEPROM()
            } label: {
                HStack {
                    Image(systemName: "arrow.up.circle.fill")
                    VStack(alignment: .leading) {
                        Text("Send EEPROM to 3DS")
                            .font(.system(.body, design: .rounded, weight: .medium))
                        Text("Upload pweep.rom to pkwbridge")
                            .font(.system(.caption2, design: .rounded))
                            .foregroundStyle(.white.opacity(0.5))
                    }
                    Spacer()
                    if isTransferring {
                        ProgressView()
                            .tint(.cyan)
                    }
                }
                .foregroundStyle(.white)
                .padding(12)
                .glassEffect(.regular, in: .rect(cornerRadius: 12))
            }
            .buttonStyle(.plain)
            .disabled(isTransferring)

            // Receive EEPROM from 3DS
            Button {
                receiveEEPROM()
            } label: {
                HStack {
                    Image(systemName: "arrow.down.circle.fill")
                    VStack(alignment: .leading) {
                        Text("Receive EEPROM from 3DS")
                            .font(.system(.body, design: .rounded, weight: .medium))
                        Text("Download pweep.rom from pkwbridge")
                            .font(.system(.caption2, design: .rounded))
                            .foregroundStyle(.white.opacity(0.5))
                    }
                    Spacer()
                    if isTransferring {
                        ProgressView()
                            .tint(.cyan)
                    }
                }
                .foregroundStyle(.white)
                .padding(12)
                .glassEffect(.regular, in: .rect(cornerRadius: 12))
            }
            .buttonStyle(.plain)
            .disabled(isTransferring)
        }
        .padding(16)
        .glassEffect(.regular, in: .rect(cornerRadius: 16))
    }

    // MARK: - Protocol Info

    private var protocolInfo: some View {
        VStack(spacing: 8) {
            Image(systemName: "info.circle")
                .foregroundStyle(.white.opacity(0.3))
            Text("3DS must be running pkwbridge with WiFi server enabled.\nFind your 3DS IP in System Settings → Internet.")
                .font(.system(.caption2, design: .rounded))
                .foregroundStyle(.white.opacity(0.3))
                .multilineTextAlignment(.center)
        }
        .padding(.bottom, 20)
    }

    // MARK: - Network Actions

    private func connect() {
        guard !ipAddress.isEmpty else {
            errorMessage = "Please enter the 3DS IP address"
            showError = true
            return
        }

        let portNum = Int(port) ?? 8000
        statusMessage = "Connecting to \(ipAddress):\(portNum)..."
        isTransferring = true

        Task {
            let success = await emulator.connectToPKWBridge(address: ipAddress, port: portNum)
            await MainActor.run {
                isTransferring = false
                if success {
                    isConnected = true
                    statusMessage = "Connected to 3DS pkwbridge"
                    connectionInfo = ConnectionInfo(device: "PKWBridge (3DS)", version: "latest")
                } else {
                    statusMessage = "Connection failed — check IP and port"
                    errorMessage = "Could not connect to \(ipAddress):\(portNum)\nMake sure pkwbridge is running on your 3DS."
                    showError = true
                }
            }
        }
    }

    private func disconnect() {
        isConnected = false
        connectionInfo = nil
        statusMessage = "Disconnected"
    }

    private func sendEEPROM() {
        guard isConnected else { return }
        isTransferring = true
        statusMessage = "Sending EEPROM to 3DS..."

        Task {
            let success = await emulator.sendEEPROMToPKWBridge(address: ipAddress, port: Int(port) ?? 8000)
            await MainActor.run {
                isTransferring = false
                if success {
                    statusMessage = "EEPROM sent successfully!"
                } else {
                    statusMessage = "Transfer failed"
                    errorMessage = "Failed to send EEPROM data to 3DS"
                    showError = true
                }
            }
        }
    }

    private func receiveEEPROM() {
        guard isConnected else { return }
        isTransferring = true
        statusMessage = "Receiving EEPROM from 3DS..."

        Task {
            let success = await emulator.receiveEEPROMFromPKWBridge(address: ipAddress, port: Int(port) ?? 8000)
            await MainActor.run {
                isTransferring = false
                if success {
                    statusMessage = "EEPROM received! Restart emulator to apply."
                } else {
                    statusMessage = "Transfer failed"
                    errorMessage = "Failed to receive EEPROM data from 3DS"
                    showError = true
                }
            }
        }
    }
}
