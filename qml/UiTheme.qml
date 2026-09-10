import QtQuick

QtObject {
    readonly property SystemPalette system: SystemPalette { colorGroup: SystemPalette.Active }
    readonly property bool dark: system.window.hslLightness < 0.5
    readonly property color canvas: dark ? "#202124" : "#f5f6f8"
    readonly property color surface: dark ? "#292b2f" : "#ffffff"
    readonly property color inset: dark ? "#232529" : "#f1f3f6"
    readonly property color hover: dark ? "#36393f" : "#e9edf3"
    readonly property color line: dark ? "#3b3e45" : "#e1e5eb"
    readonly property color text: dark ? "#eceef2" : "#242a35"
    readonly property color muted: dark ? "#a4aab6" : "#687183"
    readonly property color accent: dark ? "#53c9e4" : "#3275e8"
    readonly property color accentText: dark ? "#10262d" : "#ffffff"
    readonly property color accentSoft: dark ? "#243e48" : "#eaf1ff"
    readonly property color success: dark ? "#77d5ad" : "#218560"
    readonly property color danger: dark ? "#f09b9b" : "#c24a52"
}
