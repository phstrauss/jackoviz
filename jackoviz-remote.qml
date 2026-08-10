/*
 * jackoviz-remote.qml — vertical mockup UI for the jackoviz remote controller.
 * No gRPC / process logic yet; controls are interactive placeholders.
 * Style: Qt Quick Controls Imagine (set in jackoviz-remote.cpp).
 * Density: fonts/spacing ~80% of the original mockup.
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    title: "jackoviz remote"
    width: 288
    height: 670
    visible: true
    font.pixelSize: 11

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
    property string statusText: "OK"

    header: ToolBar {
        contentHeight: 32
        leftPadding: 10
        rightPadding: 10
        Label {
            anchors.centerIn: parent
            text: "jackoviz remote"
            font.pixelSize: 12
            font.bold: true
        }
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: 12
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: parent.width
            spacing: 9

            Label { text: "FFT size"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: fftSizeBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.fftSizes
                currentIndex: 2 // 4096
            }

            CheckBox {
                id: fastModeBox
                Layout.fillWidth: true
                font.pixelSize: 11
                text: "Fast mode (spectrum and scope only)"
            }

            Button {
                id: launchButton
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                padding: 6
                font.pixelSize: 11
                text: root.launched ? "Quit jackoviz" : "Launch jackoviz"
                onClicked: {
                    root.launched = !root.launched
                    console.log(
                        root.launched
                            ? ("Launch mock: jackoviz -n " + fftSizeBox.currentText
                               + (fastModeBox.checked ? " --fast" : ""))
                            : "Quit mock: jackoviz")
                }
            }

            Label { text: "JACK capture port"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: jackPortBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.jackPorts
                currentIndex: 1
            }

            Label { text: "View"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: viewModeBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.viewModes
                currentIndex: 3 // 3D spectrogram
            }

            Label { text: "Max frequency"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: maxFreqBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.maxFreqs
                currentIndex: 2 // 8000 Hz
            }

            Label { text: "dB ceiling"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: dbCeilBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.dbCeils
                currentIndex: 2 // -20 dB
            }

            Label { text: "dB floor"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: dbFloorBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.dbFloors
                currentIndex: 0 // -100 dB
            }

            Label {
                text: "Kaiser β  (" + kaiserSlider.value.toFixed(1) + ")"
                font.bold: true
                font.pixelSize: 11
            }
            Slider {
                id: kaiserSlider
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                from: 1.0
                to: 10.0
                stepSize: 0.1
                value: 4.5
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "1.0"; opacity: 0.6; font.pixelSize: 9 }
                Item { Layout.fillWidth: true }
                Label { text: "10.0"; opacity: 0.6; font.pixelSize: 9 }
            }

            Label { text: "Line width"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: lineWidthBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.lineWidths
                currentIndex: 1 // 2 px
            }

            Button {
                id: pauseButton
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                padding: 6
                font.pixelSize: 11
                text: root.paused ? "Resume" : "Pause"
                onClicked: {
                    root.paused = !root.paused
                    console.log(root.paused ? "Pause mock" : "Resume mock")
                }
            }

            Label {
                id: statusLabel
                Layout.fillWidth: true
                Layout.topMargin: 4
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                text: "Status: " + root.statusText
                opacity: root.statusText === "OK" ? 0.7 : 1.0
            }

            Item { Layout.preferredHeight: 6 }
        }
    }
}
