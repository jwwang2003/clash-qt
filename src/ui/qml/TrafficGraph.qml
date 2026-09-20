import QtQuick
import QtGraphs

Item {
    id: root
    implicitWidth: 540
    implicitHeight: 220
    required property ValueAxis timeAxis
    required property ValueAxis rateAxis
    required property GraphsTheme graphTheme
    required property AreaSeries downloadSeries
    required property AreaSeries uploadSeries
    property string emptyMessage: qsTr("Waiting for traffic samples…")
    property color hintColor: "#808080"
    property color hoverColor: "#808080"
    property bool animate: false
    signal frameRequested()
    FrameAnimation {
        running: root.animate && root.visible
        onTriggered: root.frameRequested()
    }
    signal sampleHovered(real fraction)
    signal hoverEnded()

    GraphsView {
        id: graph
        objectName: "trafficGraphsPlot"
        anchors.fill: parent
        theme: root.graphTheme
        axisX: root.timeAxis
        axisY: root.rateAxis
        marginLeft: 4
        marginRight: 12
        marginTop: 8
        marginBottom: 2
        shadowVisible: false
        Component.onCompleted: {
            addSeries(root.downloadSeries)
            addSeries(root.uploadSeries)
        }
    }

    Text {
        anchors.centerIn: graph
        text: root.emptyMessage
        color: root.hintColor
        visible: text.length > 0
        font.pixelSize: 13
    }

    // The data itself is drawn exclusively by Qt Graphs. This transparent
    // overlay maps pointer positions to time for the QWidget detail readout.
    MouseArea {
        id: inspection
        x: graph.plotArea.x
        y: graph.plotArea.y
        width: Math.max(0, graph.plotArea.width)
        height: Math.max(0, graph.plotArea.height)
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        property real cursorX: 0
        onPositionChanged: function(mouse) {
            cursorX = Math.max(0, Math.min(width, mouse.x))
            if (width > 0)
                root.sampleHovered(cursorX / width)
        }
        onExited: root.hoverEnded()
        Rectangle {
            visible: inspection.containsMouse && root.emptyMessage.length === 0
            x: inspection.cursorX
            y: 0
            width: 1
            height: parent.height
            color: root.hoverColor
            opacity: 0.65
        }
    }
}
