/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include "amdgpu_mode.h"
#include "amdgpu_dm.h"
#include "modules/color/color_gamma.h"


#define MAX_LUT_ENTRIES 256

/*
 * Return true if the given lut is a linear mapping of values, i.e. it acts
 * like a bypass LUT.
 *
 * It is considered linear if the lut represents:
 * f(a) = (0xFF00/MAX_LUT_ENTRIES-1)a; for integer a in [0, MAX_LUT_ENTRIES)
 */
static bool __is_lut_linear(struct drm_color_lut *lut)
{
	int i;
	uint32_t max_os = 0xFF00;
	uint32_t expected;
	int delta;

	for (i = 0; i < MAX_LUT_ENTRIES; i++) {
		/* All color values should equal */
		if ((lut[i].red != lut[i].green) || (lut[i].green != lut[i].blue))
			return false;

		expected = i * max_os / (MAX_LUT_ENTRIES-1);

		/* Allow a +/-1 error. */
		delta = lut[i].red - expected;
		if (delta < -1 || 1 < delta)
			return false;
	}
	return true;
}

/**
 * amdgpu_dm_set_regamma_lut: Set regamma lut for the given CRTC.
 * @crtc: amdgpu_dm crtc state
 *
 * Update the underlying dc_stream_state's output transfer function (OTF) in
 * preparation for hardware commit. If no lut is specified by user, we default
 * to SRGB.
 *
 * RETURNS:
 * 0 on success, -ENOMEM if memory cannot be allocated to calculate the OTF.
 */
int amdgpu_dm_set_regamma_lut(struct dm_crtc_state *crtc)
{
	struct drm_property_blob *blob = crtc->base.gamma_lut;
	struct dc_stream_state *stream = crtc->stream;
	struct drm_color_lut *lut;
	struct dc_gamma *gamma;
	enum dc_transfer_func_type old_type = stream->out_transfer_func->type;

	uint32_t r, g, b;
	int i;
	bool ret;

	if (!blob) {
		/* By default, use the SRGB predefined curve.*/
		stream->out_transfer_func->type = TF_TYPE_PREDEFINED;
		stream->out_transfer_func->tf = TRANSFER_FUNCTION_SRGB;
		return 0;
	}

	lut = (struct drm_color_lut *)blob->data;

	if (__is_lut_linear(lut)) {
		/* Set to bypass if lut is set to linear */
		stream->out_transfer_func->type = TF_TYPE_BYPASS;
		stream->out_transfer_func->tf = TRANSFER_FUNCTION_LINEAR;
		return 0;
	}

	gamma = dc_create_gamma();
	if (!gamma)
		return -ENOMEM;

	gamma->num_entries = MAX_LUT_ENTRIES;
	gamma->type = GAMMA_RGB_256;

	/* Truncate, and store in dc_gamma for output tf calculation */
	for (i = 0; i < gamma->num_entries; i++) {
		r = drm_color_lut_extract(lut[i].red, 16);
		g = drm_color_lut_extract(lut[i].green, 16);
		b = drm_color_lut_extract(lut[i].blue, 16);

		gamma->entries.red[i] = dal_fixed31_32_from_int(r);
		gamma->entries.green[i] = dal_fixed31_32_from_int(g);
		gamma->entries.blue[i] = dal_fixed31_32_from_int(b);
	}

	/* Call color module to translate into something DC understands. Namely
	 * a transfer function.
	 */
	stream->out_transfer_func->type = TF_TYPE_DISTRIBUTED_POINTS;
	ret = mod_color_calculate_regamma_params(stream->out_transfer_func,
						 gamma, true);
	dc_gamma_release(&gamma);
	if (!ret) {
		stream->out_transfer_func->type = old_type;
		DRM_ERROR("Out of memory when calculating regamma params\n");
		return -ENOMEM;
	}

	return 0;
}

/**
 * amdgpu_dm_set_ctm: Set the color transform matrix for the given CRTC.
 * @crtc: amdgpu_dm crtc state
 *
 * Update the underlying dc_stream_state's gamut remap matrix in preparation
 * for hardware commit. If no matrix is specified by user, gamut remap will be
 * disabled.
 */
void amdgpu_dm_set_ctm(struct dm_crtc_state *crtc)
{

	struct drm_property_blob *blob = crtc->base.ctm;
	struct dc_stream_state *stream = crtc->stream;
	struct drm_color_ctm *ctm;
	int i;

	if (!blob) {
		stream->gamut_remap_matrix.enable_remap = false;
		return;
	}

	stream->gamut_remap_matrix.enable_remap = true;
	ctm = (struct drm_color_ctm *)blob->data;
	/*
	 * DRM gives a 3x3 matrix, but DC wants 3x4. Assuming we're operating
	 * with homogeneous coordinates, augment the matrix with 0's.
	 *
	 * The format provided is S31.32, which is the same as our fixed31_32.
	 */
	for (i = 0; i < 12; i++) {
		/* Skip 4th element */
		if (i % 4 == 3) {
			stream->gamut_remap_matrix.matrix[i] = dal_fixed31_32_zero;
			continue;
		}
		/* csc[i] = ctm[i - floor(i/4)] */
		stream->gamut_remap_matrix.matrix[i].value = ctm->matrix[i - (i/4)];
	}
}


/**
 * amdgpu_dm_set_degamma_lut: Set degamma lut for the given CRTC.
 * @crtc: amdgpu_dm crtc state
 *
 * Update the underlying dc_stream_state's input transfer function (ITF) in
 * preparation for hardware commit. If no lut is specified by user, we default
 * to SRGB degamma.
 *
 * Currently, we only support degamma bypass, or preprogrammed SRGB degamma.
 * Programmable degamma is not supported, and an attempt to do so will return
 * -EINVAL.
 *
 * RETURNS:
 * 0 on success, -EINVAL if custom degamma curve is given.
 */
int amdgpu_dm_set_degamma_lut(struct drm_crtc_state *crtc_state,
			      struct dc_plane_state *dc_plane_state)
{
	struct drm_property_blob *blob = crtc_state->degamma_lut;
	struct drm_color_lut *lut;

	if (!blob) {
		/* Default to SRGB */
		dc_plane_state->in_transfer_func->type = TF_TYPE_PREDEFINED;
		dc_plane_state->in_transfer_func->tf = TRANSFER_FUNCTION_SRGB;
		return 0;
	}

	lut = (struct drm_color_lut *)blob->data;
	if (__is_lut_linear(lut)) {
		dc_plane_state->in_transfer_func->type = TF_TYPE_BYPASS;
		dc_plane_state->in_transfer_func->tf = TRANSFER_FUNCTION_LINEAR;
		return 0;
	}

	/* Otherwise, assume SRGB, since programmable degamma is not
	 * supported.
	 */
	dc_plane_state->in_transfer_func->type = TF_TYPE_PREDEFINED;
	dc_plane_state->in_transfer_func->tf = TRANSFER_FUNCTION_SRGB;
	return -EINVAL;
}

