/*
   FreeRDP: A Remote Desktop Protocol Implementation
   RemoteFX Codec Library - NEON Optimizations

   Copyright 2011 Martin Fleisz <martin.fleisz@thincast.com>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef FREERDP_LIB_CODEC_RFX_NEON_H
#define FREERDP_LIB_CODEC_RFX_NEON_H

#include <winpr/sysinfo.h>

#include <freerdp/codec/rfx.h>
#include <freerdp/api.h>

FREERDP_LOCAL void rfx_init_neon_int(RFX_CONTEXT* WINPR_RESTRICT context);
static inline void rfx_init_neon(RFX_CONTEXT* WINPR_RESTRICT context)
{
	if (!IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE))
		return;

	rfx_init_neon_int(context);
}

#endif /* FREERDP_LIB_CODEC_RFX_NEON_H */
