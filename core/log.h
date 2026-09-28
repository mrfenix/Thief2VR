#pragma once

// Appends a line to thief2vr.log next to the game exe. printf-style.
void LogInit();
void Log(const char* fmt, ...);
