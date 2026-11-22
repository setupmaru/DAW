#pragma once

// Application commands (menus, shortcuts and buttons all invoke these).
namespace CommandIDs
{
enum : int
{
    newSession = 0x2000,
    openSession,
    saveSession,
    saveSessionAs,
    importAudio,
    bounce,
    audioSettings,
    rescanPlugins,

    undo = 0x2100,
    redo,
    deleteSelection,
    split,
    duplicate,
    selectAll,

    newTrack = 0x2200,
    newStereoTrack,
    deleteTrack,

    playStop = 0x2300,
    record,
    returnToZero,
    toggleLoop,
    toggleClick,

    toolTrim = 0x2400,
    toolSelect,
    toolGrab,
    modeSlip,
    modeGrid,
    zoomIn,
    zoomOut,
    zoomFit,
    toggleMixEdit,
    toggleCounter,
};
}
