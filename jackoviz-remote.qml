/*
 * jackoviz-remote.qml — vertical UI for the jackoviz remote controller.
 * Launch/FFT/Fast via fork+exec; JACK ports + runtime settings via gRPC.
 * Style: Imagine (set in jackoviz-remote.cpp).
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    title: "jackoviz remote"
    width: 260
    height: 670
    visible: true
    font.pixelSize: 11

    readonly property var fftSizes: ["1024", "2048", "4096", "8192"]
    readonly property var viewModesFull: [
        "Oscilloscope",
        "1D spectrum",
        "2D spectrogram",
        "3D spectrogram"
    ]
    readonly property var viewModesFast: [
        "Oscilloscope",
        "1D spectrum"
    ]
    readonly property var maxFreqs: [
        "4000 Hz", "6000 Hz", "8000 Hz", "12000 Hz", "16000 Hz", "20000 Hz"
    ]
    readonly property var maxFreqValues: [4000, 6000, 8000, 12000, 16000, 20000]
    readonly property var dbCeils: ["0 dB", "-10 dB", "-20 dB"]
    readonly property var dbCeilValues: [0, -10, -20]
    readonly property var dbFloors: [
        "-100 dB", "-110 dB", "-120 dB", "-130 dB", "-140 dB"
    ]
    readonly property var dbFloorValues: [-100, -110, -120, -130, -140]
    readonly property var lineWidths: ["1 px", "2 px"]
    readonly property var lineWidthValues: [1.0, 2.0]

    readonly property bool launched: controller.launched
    readonly property bool paused: controller.paused
    readonly property string statusText: controller.statusText

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
                enabled: !root.launched
            }

            CheckBox {
                id: fastModeBox
                Layout.fillWidth: true
                font.pixelSize: 11
                text: "Fast mode (spectrum and scope only)"
                enabled: !root.launched
                onCheckedChanged: {
                    if (checked && viewModeBox.currentIndex > 1)
                        viewModeBox.currentIndex = 1
                }
            }

            Button {
                id: launchButton
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                padding: 6
                font.pixelSize: 11
                text: root.launched ? "Quit jackoviz" : "Launch jackoviz"
                onClicked: {
                    if (root.launched)
                        controller.quitJackoviz()
                    else
                        controller.launch(
                            parseInt(fftSizeBox.currentText, 10),
                            fastModeBox.checked)
                }
            }

            Label { text: "JACK capture port"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: jackPortBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: controller.jackPorts
                onActivated: controller.connectJackPort(currentText)

                function selectPreferredPort() {
                    const ports = controller.jackPorts
                    if (!ports || ports.length === 0)
                        return
                    let i = ports.indexOf(currentText)
                    if (i < 0)
                        i = ports.indexOf("system:capture_1")
                    if (i < 0)
                        i = 0
                    currentIndex = i
                }

                Component.onCompleted: {
                    selectPreferredPort()
                    controller.connectJackPort(currentText)
                }

                Connections {
                    target: controller
                    function onJackPortsChanged() {
                        jackPortBox.selectPreferredPort()
                    }
                }
            }

            Label { text: "View"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: viewModeBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: fastModeBox.checked ? root.viewModesFast : root.viewModesFull
                currentIndex: 3 // 3D spectrogram (clamped if fast)
                onActivated: controller.setViewMode(currentIndex)
                Component.onCompleted: controller.setViewMode(currentIndex)
            }

            Label { text: "Max frequency"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: maxFreqBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.maxFreqs
                currentIndex: 2 // 8000 Hz
                onActivated: controller.setPlotFreq(root.maxFreqValues[currentIndex])
                Component.onCompleted:
                    controller.setPlotFreq(root.maxFreqValues[currentIndex])
            }

            Label { text: "dB ceiling"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: dbCeilBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.dbCeils
                currentIndex: 2 // -20 dB
                onActivated: controller.setDbCeil(root.dbCeilValues[currentIndex])
                Component.onCompleted:
                    controller.setDbCeil(root.dbCeilValues[currentIndex])
            }

            Label { text: "dB floor"; font.bold: true; font.pixelSize: 11 }
            ComboBox {
                id: dbFloorBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.dbFloors
                currentIndex: 0 // -100 dB
                onActivated: controller.setDbFloor(root.dbFloorValues[currentIndex])
                Component.onCompleted:
                    controller.setDbFloor(root.dbFloorValues[currentIndex])
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
                onMoved: kaiserDebounce.restart()
                Component.onCompleted: controller.setKaiserBeta(value)
            }
            Timer {
                id: kaiserDebounce
                interval: 120
                onTriggered: controller.setKaiserBeta(kaiserSlider.value)
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
                onActivated: controller.setLineWidth(root.lineWidthValues[currentIndex])
                Component.onCompleted:
                    controller.setLineWidth(root.lineWidthValues[currentIndex])
            }

            Button {
                id: pauseButton
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                padding: 6
                font.pixelSize: 11
                enabled: root.launched
                text: root.paused ? "Resume" : "Pause"
                onClicked: controller.setPaused(!root.paused)
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
