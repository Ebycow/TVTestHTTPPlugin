#pragma once

static const int      HTTP_PORT_DEFAULT            = 40152;
static const UINT_PTR TIMER_ID                     = 40152u;
static const UINT     TIMER_MS                     = 50u;
static const int      PROGRAM_REFRESH_INTERVAL     = 40;

// TTRec WM_APP message identifiers
static const UINT WM_TTREC_GET_MSGVER                 = WM_APP + 50;
static const UINT WM_TTREC_EVENT_PROGRAMGUIDE_COMMAND = WM_APP + 53;
static const UINT TTREC_CURRENT_MSGVER                = 1u;
static const UINT TTREC_COMMAND_RESERVE_DEFAULT       = 2u;
