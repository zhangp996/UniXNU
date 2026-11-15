// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace SmUfsBlockVfsProof {
inline bool completion(uint64_t requested, int error, int expectedError,
                       int64_t residual, uint64_t expectedActual, uint32_t callbacks)
{
	return callbacks == 1 && error == expectedError && residual >= 0 &&
	       uint64_t(residual) <= requested && expectedActual <= requested &&
	       requested - uint64_t(residual) == expectedActual;
}
inline bool settled(bool workerReturned, bool submitted, uint32_t callbacks,
                    uint32_t callbackActive, uint32_t inflight)
{
	return workerReturned && callbacks == (submitted ? 1u : 0u) && !callbackActive && !inflight;
}
inline bool pathname(bool root, bool devfs, bool context, bool identity,
                     int openError, int readError, int closeError, int64_t residual)
{
	return root && devfs && context && identity && !openError && !readError && !closeError && residual == 0;
}
inline bool result(bool block, bool vfs, bool blocked, bool closed, bool stable, bool isolated)
{
	return block && vfs && !blocked && closed && stable && isolated;
}
inline bool blockOnly(bool block, bool vfs, bool blocked, bool closed, bool stable, bool isolated)
{
	return block && !vfs && blocked && closed && stable && isolated;
}
}
