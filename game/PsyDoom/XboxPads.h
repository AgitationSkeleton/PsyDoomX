#pragma once

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom Xbox: keeping controllers alive, and saying what they are doing.
//
// Pads that are not Microsoft's own - cheap third party ones, Logitech's wireless ones - were reported going dead once a
// game had loaded, and only unplugging and replugging brought them back. The cause is in the libraries underneath, not
// in the game, and it takes two faults together:
//
//  - SDL reads each pad with one interrupt transfer that it queues again every time it completes. If a transfer ever
//    completes with an error it simply returns, without queueing another. Nothing ever starts it again, so from then on
//    that pad sends nothing at all.
//  - The USB host controller halts an endpoint when a transfer on it fails, and nxdk's driver never clears the halt.
//    Queueing another transfer behind a halted endpoint would not have helped on its own.
//
// One bad packet is therefore permanent. A Microsoft pad rarely produces one; a third party pad, or a wireless receiver
// whose link hiccups, can - and heavy disc reading while a level loads is when the bus is busiest. Replugging fixed it
// because a new device gets a new pipe.
//
// This module watches every pad's read pipe from the game thread and, when it finds one stopped by an error, clears the
// halt and starts it again. It also reports each pad as it arrives - vendor, product, XID type, endpoint layout - and
// counts reads, errors and restarts, so a report from a pad that still misbehaves says why rather than just that it did.
//------------------------------------------------------------------------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>

namespace XboxPads {

// Start a section of 'pads.log' for this process - 'launcher', or the game being started - so a log of several sessions
// can be read. Everything this module reports goes to the relay and, apart from the routine five second summaries, to
// that file beside the executable as well: a pad tested with no machine listening still leaves a record. The file is
// cut back when it grows past a few hundred kilobytes.
void beginSession(const char* const name) noexcept;

// Look after the pads. Call from the thread that calls 'usbh_pooling_hubs', just after it: that is the thread that can
// disconnect a device, so doing both on it means a pad cannot disappear in the middle of being looked at. Cheap when
// nothing is wrong - a walk over at most four devices.
void service() noexcept;

// Stop the USB host controller before this process hands the console to another XBE.
//
// The controller does its own memory access, following lists of transfers it was given. A quick reboot into another XBE
// does not stop it, so it went on reading and writing memory the next program was being loaded into until that program
// got round to resetting it. Stopping it here means the next process starts from a quiet bus.
void shutdownForRelaunch() noexcept;

// A line per pad for an on screen diagnostic: 'P1 045E:0289 rd 1203 err 0 rst 0'. Returns how many pads were described.
int32_t describe(char* const out, const size_t outSize) noexcept;

}   // namespace XboxPads
