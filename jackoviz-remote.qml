/*
 * jackoviz-remote.qml — vertical mockup UI for the jackoviz remote controller.
 * No gRPC / process logic yet; controls are interactive placeholders.
 * Style: Qt Quick Controls Imagine (set in jackoviz-remote.cpp).
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    title: "jackoviz remote"
    width: 360
    height: 720
    visible: true

    readonly property var fftSizes: ["1024", "2048", "4096", "8192"]
    readonly property var jackPorts: [
        "(none)",
        "system:capture_1",
        "system:capture_2",
        "system:capture_3",
        "system:capture_4"
    ]
    readonly property var viewModes: [
        "Oscilloscope",
        "1D spectrum",
        "2D spectrogram",
        "3D spectrogram"
    ]
    readonly property var maxFreqs: [
        "4000 Hz", "6000 Hz", "8000 Hz", "12000 Hz", "16000 Hz", "20000 Hz"
    ]
    readonly property var dbCeils: ["0 dB", "-10 dB", "-20 dB"]
    readonly property var dbFloors: [
        "-100 dB", "-110 dB", "-120 dB", "-130 dB", "-140 dB"
    ]
    readonly property var lineWidths: ["1 px", "2 px"]

    property bool paused: false
    property bool launched: false

    header: ToolBar {
        contentHeight: 40
        Label {
            anchors.centerIn: parent
            text: "jackoviz remote"
            font.pixelSize: 15
            font.bold: true
        }
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: 16
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: parent.width
            spacing: 12

            Label { text: "FFT size"; font.bold: true }
            ComboBox {
                id: fftSizeBox
                Layout.fillWidth: true
                model: root.fftSizes
                currentIndex: 2 // 4096
            }

            Button {
                id: launchButton
                Layout.fillWidth: true
                text: root.launched ? "Launched" : "Launch"
                enabled: !root.launched
                onClicked: {
                    root.launched = true
                    console.log("Launch mock: jackoviz -n", fftSizeBox.currentText)
                }
            }

            Label { text: "JACK capture port"; font.bold: true }
            ComboBox {
                id: jackPortBox
                Layout.fillWidth: true
                model: root.jackPorts
                currentIndex: 1
            }

            Label { text: "View"; font.bold: true }
            ComboBox {
                id: viewModeBox
                Layout.fillWidth: true
                model: root.viewModes
                currentIndex: 3 // 3D spectrogram
            }

            Label { text: "Max frequency"; font.bold: true }
            ComboBox {
                id: maxFreqBox
                Layout.fillWidth: true
                model: root.maxFreqs
                currentIndex: 2 // 8000 Hz
            }

            Label { text: "dB ceiling"; font.bold: true }
            ComboBox {
                id: dbCeilBox
                Layout.fillWidth: true
                model: root.dbCeils
                currentIndex: 2 // -20 dB
            }

            Label { text: "dB floor"; font.bold: true }
            ComboBox {
                id: dbFloorBox
                Layout.fillWidth: true
                model: root.dbFloors
                currentIndex: 0 // -100 dB
            }

            Label {
                text: "Kaiser β  (" + kaiserSlider.value.toFixed(1) + ")"
                font.bold: true
            }
            Slider {
                id: kaiserSlider
                Layout.fillWidth: true
                from: 1.0
                to: 10.0
                stepSize: 0.1
                value: 4.5
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "1.0"; opacity: 0.6; font.pixelSize: 11 }
                Item { Layout.fillWidth: true }
                Label { text: "10.0"; opacity: 0.6; font.pixelSize: 11 }
            }

            Label { text: "Line width"; font.bold: true }
            ComboBox {
                id: lineWidthBox
                Layout.fillWidth: true
                model: root.lineWidths
                currentIndex: 1 // 2 px
            }

            Button {
                id: pauseButton
                Layout.fillWidth: true
                text: root.paused ? "Resume" : "Pause"
                onClicked: {
                    root.paused = !root.paused
                    console.log(root.paused ? "Pause mock" : "Resume mock")
                }
            }

            Button {
                id: quitButton
                Layout.fillWidth: true
                text: "Quit"
                onClicked: Qt.quit()
            }

            Item { Layout.preferredHeight: 8 }
        }
    }
}
