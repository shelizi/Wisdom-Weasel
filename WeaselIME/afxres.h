// Shim for builds without MFC: afxres.h -> winres.h
#pragma once
#include <winres.h>
#ifndef IDC_STATIC
#define IDC_STATIC (-1)
#endif
