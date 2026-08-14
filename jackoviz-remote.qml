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
    height: 728
    minimumWidth: 260
    maximumWidth: 260
    minimumHeight: 728
    maximumHeight: 728
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
    readonly property bool jackRunning: controller.jackRunning
    readonly property string appVersion: controller.appVersion

    property int syncStep: 0
    readonly property int syncStepCount: 8

    Dialog {
        id: aboutDialog
        title: qsTr("About jackoviz")
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.NoButton
        width: root.width
        leftPadding: 12
        rightPadding: 12
        topPadding: 12
        bottomPadding: 12

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: "jackoviz " + root.appVersion
                font.bold: true
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Realtime JACK → FFTW3 → Datoviz spectrogram, with this remote GUI")
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                Layout.topMargin: 4
                text: "Copyright © 2026 Philippe Strauss"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                opacity: 0.8
            }
            Button {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                Layout.topMargin: 4
                padding: 6
                font.pixelSize: 11
                text: qsTr("OK")
                onClicked: aboutDialog.accept()
            }
        }
    }

    function pushAllSettings() {
        if (!root.launched)
            return
        syncStep = 0
        staggeredSync.stop()
        sendSyncStep()
        if (root.launched && syncStep < syncStepCount)
            staggeredSync.start()
    }

    function sendSyncStep() {
        if (!root.launched || syncStep >= syncStepCount) {
            staggeredSync.stop()
            return
        }

        switch (syncStep) {
        case 0:
            controller.setViewMode(
                fastModeBox.checked ? Math.min(1, viewModeBox.currentIndex)
                                    : viewModeBox.currentIndex)
            break
        case 1:
            controller.setPlotFreq(root.maxFreqValues[maxFreqBox.currentIndex])
            break
        case 2:
            controller.setDbCeil(root.dbCeilValues[dbCeilBox.currentIndex])
            break
        case 3:
            controller.setDbFloor(root.dbFloorValues[dbFloorBox.currentIndex])
            break
        case 4:
            controller.setKaiserBeta(kaiserSlider.value)
            break
        case 5:
            controller.setLineWidth(root.lineWidthValues[lineWidthBox.currentIndex])
            break
        case 6:
            controller.setPaused(root.paused)
            break
        case 7:
            controller.connectJackPort(jackPortBox.currentText)
            break
        }
        syncStep++
    }

    Timer {
        id: postLaunchSync
        interval: 5000
        repeat: false
        onTriggered: root.pushAllSettings()
    }

    Timer {
        id: staggeredSync
        interval: 200
        repeat: true
        onTriggered: {
            root.sendSyncStep()
            if (root.syncStep >= root.syncStepCount || !root.launched)
                stop()
        }
    }

    Connections {
        target: controller
        function onLaunchedChanged() {
            if (controller.launched) {
                staggeredSync.stop()
                postLaunchSync.restart()
            } else {
                postLaunchSync.stop()
                staggeredSync.stop()
            }
        }
        function onViewModeIndexChanged() {
            if (viewModeBox.currentIndex !== controller.viewModeIndex)
                viewModeBox.currentIndex = controller.viewModeIndex
        }
    }

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
                enabled: !root.launched
                Component.onCompleted: {
                    const i = root.fftSizes.indexOf(String(controller.fftSize))
                    currentIndex = i >= 0 ? i : 2
                    controller.fftSize = parseInt(currentText, 10)
                }
                onActivated: controller.fftSize = parseInt(currentText, 10)
            }

            CheckBox {
                id: fastModeBox
                Layout.fillWidth: true
                font.pixelSize: 11
                text: "Fast mode (spectrum and scope only)"
                enabled: !root.launched
                Component.onCompleted: checked = controller.fastMode
                onCheckedChanged: {
                    controller.fastMode = checked
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
                    let i = ports.indexOf(controller.jackPort)
                    if (i < 0)
                        i = ports.indexOf(currentText)
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
                Component.onCompleted: {
                    let i = controller.viewModeIndex
                    if (fastModeBox.checked && i > 1)
                        i = 1
                    if (i < 0 || i >= model.length)
                        i = 1
                    currentIndex = i
                    controller.setViewMode(currentIndex)
                }
                onActivated: controller.setViewMode(currentIndex)
            }

            Label {
                text: controller.plotFreqEnabled
                      ? "Max frequency"
                      : "Max frequency (n/a in scope)"
                font.bold: true
                font.pixelSize: 11
            }
            ComboBox {
                id: maxFreqBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.maxFreqs
                enabled: controller.plotFreqEnabled
                Component.onCompleted: {
                    let i = root.maxFreqValues.indexOf(controller.plotFreq)
                    if (i < 0)
                        i = 1
                    currentIndex = i
                    controller.setPlotFreq(root.maxFreqValues[currentIndex])
                }
                onActivated: controller.setPlotFreq(root.maxFreqValues[currentIndex])
            }

            Label {
                text: controller.dbRangeEnabled
                      ? "dB ceiling"
                      : "dB ceiling (n/a in scope)"
                font.bold: true
                font.pixelSize: 11
            }
            ComboBox {
                id: dbCeilBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.dbCeils
                enabled: controller.dbRangeEnabled
                Component.onCompleted: {
                    let i = root.dbCeilValues.indexOf(controller.dbCeil)
                    if (i < 0)
                        i = 2
                    currentIndex = i
                    controller.setDbCeil(root.dbCeilValues[currentIndex])
                }
                onActivated: controller.setDbCeil(root.dbCeilValues[currentIndex])
            }

            Label {
                text: controller.dbRangeEnabled
                      ? "dB floor"
                      : "dB floor (n/a in scope)"
                font.bold: true
                font.pixelSize: 11
            }
            ComboBox {
                id: dbFloorBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.dbFloors
                enabled: controller.dbRangeEnabled
                Component.onCompleted: {
                    let i = root.dbFloorValues.indexOf(controller.dbFloor)
                    if (i < 0)
                        i = 2
                    currentIndex = i
                    controller.setDbFloor(root.dbFloorValues[currentIndex])
                }
                onActivated: controller.setDbFloor(root.dbFloorValues[currentIndex])
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
                Component.onCompleted: {
                    value = controller.kaiserBeta
                    controller.setKaiserBeta(value)
                }
                onMoved: kaiserDebounce.restart()
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

            Label {
                text: controller.lineWidthEnabled
                      ? "Line width"
                      : "Line width (scope / 1D only)"
                font.bold: true
                font.pixelSize: 11
            }
            ComboBox {
                id: lineWidthBox
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                font.pixelSize: 11
                model: root.lineWidths
                enabled: controller.lineWidthEnabled
                Component.onCompleted: {
                    let i = root.lineWidthValues.indexOf(controller.lineWidth)
                    if (i < 0)
                        i = 0
                    currentIndex = i
                    controller.setLineWidth(root.lineWidthValues[currentIndex])
                }
                onActivated: controller.setLineWidth(root.lineWidthValues[currentIndex])
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

            Button {
                id: aboutButton
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                padding: 6
                font.pixelSize: 11
                text: qsTr("About Jackoviz")
                onClicked: aboutDialog.open()
            }

            Label {
                id: statusLabel
                Layout.fillWidth: true
                Layout.topMargin: 4
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                text: "Status: " + root.statusText
                color: root.jackRunning ? palette.windowText : "#b00020"
                opacity: (!root.jackRunning || root.statusText !== "OK") ? 1.0 : 0.7
            }

            Item { Layout.preferredHeight: 6 }
        }
    }
}
