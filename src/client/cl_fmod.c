/*
Copyright (C) 2001-2002 A Nourai

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// cl_fmod.c -- f_modified: whether the models, sounds and palette the game
// checks are the original ones, or replacements ezQuake allows. Each is hashed
// (SHA-1) as it is loaded, and compared with ezQuake's hashes (its fmod.c).

#include "cl_local.h"
#include "sha1.h"

typedef struct
{
	const char	*name;
	uint32_t	hash[5];	// the digest as little-endian words, as ezQuake has it
} fmod_hash_t;

// the original of each file first, then the replacements allowed
static const fmod_hash_t	fmod_hashes[] =
{
	{"progs/armor.mdl",				{0x18d1b8ef, 0xfe43d973, 0xbb104313, 0xef6a9090, 0x2b048696}},
	{"progs/armor.mdl",				{0x8d264ffc, 0xfabb1e7c, 0xc81128bc, 0x21633d32, 0xfa0b594f}},	// mdlhash_debug_armor
	{"progs/armor.mdl",				{0x7772c52a, 0xdee37c13, 0xbbbc8523, 0x348525ba, 0x6e1bd649}},	// mdlhash_ruohis_armor
	{"progs/armor.mdl",				{0x5647e7b2, 0x52465746, 0xc10320fa, 0x1e92c90c, 0x27577c72}},	// mdlhash_plaguespak_armor
	{"progs/backpack.mdl",			{0x80cedaeb, 0x18f7d282, 0xa911e7c2, 0xffe609aa, 0x050a6005}},
	{"progs/backpack.mdl",			{0xe7c1f382, 0x0c3dc22e, 0xd31a04c0, 0x7251ed52, 0x45baf223}},	// mdlhash_debug_backpack
	{"progs/bolt2.mdl",				{0x14fba8de, 0x99237ae4, 0xc8806043, 0xebeea454, 0x024a99a9}},
	{"progs/end1.mdl",				{0xd1fed19f, 0x5d67c632, 0x1d72a0e6, 0x14ab39d7, 0x4bf435c4}},
	{"progs/end1.mdl",				{0x51ff9322, 0x8b31b83d, 0x8988bebf, 0x0917dce7, 0xd9578e04}},	// mdlhash_ruohis_end1
	{"progs/end2.mdl",				{0x98eee620, 0x107acd79, 0x8331620d, 0x2a9a1848, 0x64d29e1a}},
	{"progs/end2.mdl",				{0xe3a1b910, 0x28c4e36a, 0x1af69321, 0x9d19df99, 0x5bcebfcb}},	// mdlhash_unknown_end2
	{"progs/end2.mdl",				{0x2ea71e4b, 0x0ef6cf22, 0x86d53ba5, 0x1145433d, 0xe6712054}},	// mdlhash_ruohis_end2
	{"progs/end3.mdl",				{0x7f1e336d, 0xe04f4bb0, 0x4ac25a1a, 0x2caedde7, 0x09973319}},
	{"progs/end3.mdl",				{0xe3a1b910, 0x28c4e36a, 0x1af69321, 0x9d19df99, 0x5bcebfcb}},	// mdlhash_unknown_end3
	{"progs/end3.mdl",				{0x04c3eda0, 0x2b4f2534, 0x127abbca, 0x755c97cf, 0x135b6597}},	// mdlhash_ruohis_end3
	{"progs/end4.mdl",				{0xa4b572aa, 0xdd00c38d, 0x130f8da2, 0x797f0e55, 0xcca52b76}},
	{"progs/end4.mdl",				{0xe3a1b910, 0x28c4e36a, 0x1af69321, 0x9d19df99, 0x5bcebfcb}},	// mdlhash_unknown_end4
	{"progs/end4.mdl",				{0x4d5a54c1, 0x1fa8aaba, 0x600a5b2b, 0x7508ff3f, 0xc7af03de}},	// mdlhash_ruohis_end4
	{"progs/eyes.mdl",				{0x276f25a8, 0xc6b8f782, 0x3ef75d52, 0x911d163e, 0x5d79e557}},
	{"progs/g_light.mdl",			{0xcb9b9366, 0x1fd74d91, 0x9a6a82f6, 0x5d6b2e3e, 0xc458f5ac}},
	{"progs/g_light.mdl",			{0x9d78b478, 0xfb44c849, 0xc56b6d9d, 0xab737839, 0x1fcfd848}},	// mdlhash_debug_g_light
	{"progs/g_light.mdl",			{0x6231b892, 0x1f39d616, 0x1442715d, 0xfbfc2826, 0xc866d8d6}},	// mdlhash_plaguespak_g_light
	{"progs/g_light.mdl",			{0xfe2d50b8, 0x6ad56eb4, 0xe579fceb, 0x43e46f6d, 0x82f616da}},	// mdlhash_ruohis_g_light
	{"progs/g_nail.mdl",			{0x7b209f71, 0xa77cd00f, 0x91f24953, 0x932f264b, 0x350e7440}},
	{"progs/g_nail.mdl",			{0xc6619f5d, 0x55511a85, 0x6de0cc63, 0xef61171a, 0x36b1b173}},	// mdlhash_debug_g_nail
	{"progs/g_nail.mdl",			{0xcb2e31c6, 0x1f1064ab, 0x5e0be681, 0x5e658642, 0xd9411ef4}},	// mdlhash_plaguespak_g_nail
	{"progs/g_nail.mdl",			{0xbe7da37e, 0xb531272b, 0x0c7de89a, 0x26f387bd, 0x20dc31ee}},	// mdlhash_ruohis_g_nail
	{"progs/g_nail.mdl",			{0x86dd1964, 0xebe66f85, 0xf27e5b4c, 0x3230aeda, 0x10c13860}},	// mdlhash_pdp_g_nail
	{"progs/g_nail2.mdl",			{0x97cb4499, 0x88e3e11a, 0xecec6eca, 0x883f6c8f, 0xac7ecf0c}},
	{"progs/g_nail2.mdl",			{0x9cd58369, 0xe54c8527, 0xcea234b4, 0x472bd93e, 0x7dfa8655}},	// mdlhash_debug_g_nail2
	{"progs/g_nail2.mdl",			{0x7d3fe401, 0x738a4344, 0x4ab62439, 0xba7313eb, 0x6a6f8536}},	// mdlhash_plaguespak_g_nail2
	{"progs/g_nail2.mdl",			{0x38df15ca, 0xe2db4350, 0x5c065841, 0x67ec548c, 0x3db3f0b2}},	// mdlhash_ruohis_g_nail2
	{"progs/g_rock.mdl",			{0x9017dd83, 0x150d954c, 0x9b5f4544, 0x10843b72, 0x4697068e}},
	{"progs/g_rock.mdl",			{0x334e9253, 0xa5a5a252, 0x68b9a856, 0x5e226647, 0xe4baeec7}},	// mdlhash_debug_g_rock
	{"progs/g_rock.mdl",			{0xe31fc7b0, 0x35200618, 0x8ba6973c, 0x1296c555, 0xb2541bde}},	// mdlhash_plaguespak_g_rock
	{"progs/g_rock.mdl",			{0xd2e3e29a, 0x1e3a2acf, 0xa2f21d53, 0x60452add, 0xd8732afa}},	// mdlhash_ruohis_g_rock
	{"progs/g_rock.mdl",			{0x10e990fe, 0x3b401ed2, 0x9a9d71ad, 0x9085f059, 0x7710088f}},	// mdlhash_pdp_g_rock
	{"progs/g_rock2.mdl",			{0x5a47ec20, 0xd0211cdc, 0xd6b8af60, 0xaf813eab, 0xba330b5b}},
	{"progs/g_rock2.mdl",			{0xe24e10f9, 0x0f53bc41, 0x6043ee2b, 0x3d577eec, 0xbc75124c}},	// mdlhash_debug_g_rock2
	{"progs/g_rock2.mdl",			{0x674eb3e9, 0xde325927, 0xdabd4337, 0xc97a755c, 0x97f4f1f9}},	// mdlhash_plaguespak_g_rock2
	{"progs/g_rock2.mdl",			{0x54350c58, 0x6a09fd88, 0xae214b80, 0xc71d71de, 0x109d0fe6}},	// mdlhash_ruohis_g_rock2
	{"progs/g_rock2.mdl",			{0x3c4280d9, 0xd8a43a56, 0xeff931eb, 0xb76310af, 0xb28c39ad}},	// mdlhash_pdp_g_rock2
	{"progs/g_shot.mdl",			{0x35a735e1, 0xc1ee6090, 0x9f8940b5, 0x6cdefd1c, 0x7eec1d67}},
	{"progs/g_shot.mdl",			{0x7bbba828, 0x99438f98, 0x975e3747, 0x6cbc8a2b, 0xd3a64db7}},	// mdlhash_debug_g_shot
	{"progs/g_shot.mdl",			{0xd3486358, 0x4a3a3d37, 0x0efc43e4, 0x55a42f89, 0xf8078519}},	// mdlhash_plaguespak_g_shot
	{"progs/g_shot.mdl",			{0xc74cafd8, 0xbc3af902, 0xbb52b588, 0x6f6fca30, 0x5b2ab554}},	// mdlhash_ruohis_g_shot
	{"progs/g_shot.mdl",			{0x3a9f015f, 0xd817a29e, 0xdc7a87fa, 0xc6e31628, 0xaa991119}},	// mdlhash_pdp_g_shot
	{"progs/gib1.mdl",				{0x99dc9ea4, 0x6e9bf74a, 0x25710a1e, 0x701fc77b, 0x09777092}},
	{"progs/gib1.mdl",				{0x69ee06fd, 0x8bac8483, 0xf9c5a43e, 0xd7513722, 0x55d5a4ff}},	// mdlhash_debug_gib1
	{"progs/gib1.mdl",				{0x05ff57f8, 0x1073a096, 0xc7e5d055, 0x9c6c040b, 0x96d2eb8a}},	// mdlhash_ruohis_gib1
	{"progs/gib2.mdl",				{0x5ac3e29b, 0xd66358d9, 0x1044d27a, 0xb3da48ad, 0x5f1e9fbb}},
	{"progs/gib2.mdl",				{0x3251a1df, 0x50e68208, 0xeff0f797, 0x89894e71, 0x65505be3}},	// mdlhash_debug_gib2
	{"progs/gib2.mdl",				{0xdf23d687, 0x51ea9047, 0xbfdb3497, 0xed7b63db, 0x131a1bfb}},	// mdlhash_ruohis_gib2
	{"progs/gib3.mdl",				{0xc6558e61, 0x13ea4f63, 0x20c9da45, 0x0640212e, 0x7b98f350}},
	{"progs/gib3.mdl",				{0x798f0b46, 0xe55c7250, 0x882ff3f5, 0x75495a80, 0x19a3ed99}},	// mdlhash_debug_gib3
	{"progs/gib3.mdl",				{0xff5df01e, 0x7606952f, 0xdd36c784, 0xa0d72d33, 0x066afa33}},	// mdlhash_ruohis_gib3
	{"progs/grenade.mdl",			{0x60dfffb8, 0xfc871f0c, 0xd9f3c325, 0x61aadcaf, 0x0ec37cbf}},
	{"progs/grenade.mdl",			{0xdaf5db12, 0x41d4fc02, 0x764dd35a, 0xa4490888, 0xd5d26cea}},	// mdlhash_debug_grenade
	{"progs/grenade.mdl",			{0xe3a1b910, 0x28c4e36a, 0x1af69321, 0x9d19df99, 0x5bcebfcb}},	// mdlhash_plaguespak_grenade
	{"progs/grenade.mdl",			{0xece5571e, 0xa35d61e8, 0xe2b6df49, 0xf35372d9, 0x7ac78910}},	// mdlhash_ruohis_grenade
	{"progs/invisibl.mdl",			{0x23e2f389, 0x8479657f, 0x437e0d25, 0xee100bae, 0xbad6a775}},
	{"progs/invisibl.mdl",			{0xe5acae60, 0x8b2fe8fd, 0xb8ef8e78, 0x8d236ae4, 0xc3db0be3}},	// mdlhash_debug_invisibl
	{"progs/invisibl.mdl",			{0xb7507e7e, 0x84dc7019, 0x8f5f1c39, 0xc0df2979, 0xe86b93dd}},	// mdlhash_ruohis_invisibl
	{"progs/invulner.mdl",			{0x987ee175, 0xfd0d4f35, 0x06d2641c, 0x725c0dc8, 0x871f537a}},
	{"progs/invulner.mdl",			{0x57b00a3e, 0x009afa6d, 0xa4c2c8cb, 0xa7b0eccc, 0xa9e57049}},	// mdlhash_debug_invulner
	{"progs/invulner.mdl",			{0xddcca118, 0x9e143d41, 0xa2a8dc57, 0x82744eb7, 0x09af301b}},	// mdlhash_ruohis_invulner
	{"progs/missile.mdl",			{0x9adfeee8, 0x185872c1, 0xb3bb36f8, 0x996e29ab, 0xd46ab2a9}},
	{"progs/missile.mdl",			{0x2ae7a078, 0xc39393d4, 0x73576788, 0x242699d2, 0x8f190bfd}},	// mdlhash_debug_missile
	{"progs/missile.mdl",			{0xe047b3ec, 0xad03d2e2, 0x2a146207, 0x99e1f2df, 0xfb229f42}},	// mdlhash_plaguespak_missile
	{"progs/missile.mdl",			{0x7e844aca, 0xb1b07ef9, 0x3d8994d8, 0xe6b4d14e, 0x56c49858}},	// mdlhash_ruohis_missile
	{"progs/quaddama.mdl",			{0x2760f663, 0x32dc8405, 0x057563df, 0x9614c3a7, 0x0125949b}},
	{"progs/quaddama.mdl",			{0x9010a156, 0x1b63addb, 0xbc9bd9e3, 0xff8d6e4e, 0xcecd1260}},	// mdlhash_debug_quaddama
	{"progs/quaddama.mdl",			{0xdb93cf6c, 0x7006a0b8, 0x90b28c56, 0xf97afba7, 0x423699cb}},	// mdlhash_ruohis_quaddama
	{"progs/s_spike.mdl",			{0xf308f8e5, 0xcdc242e2, 0x4f71b01f, 0xafb9880a, 0x52198e9f}},
	{"progs/s_spike.mdl",			{0xb159e4cc, 0xbc5dccf0, 0x656e93ab, 0x3e72dd24, 0x10446fc6}},	// mdlhash_debug_s_spike
	{"progs/s_spike.mdl",			{0xe7c61b11, 0x703a7f30, 0x0051a5da, 0xb84a5bd1, 0xe23645ac}},	// mdlhash_plaguespak_s_spike
	{"progs/s_spike.mdl",			{0x40bf84c5, 0xcab85a9c, 0x498baa24, 0xbe07d9c6, 0x6b666756}},	// mdlhash_ruohis_s_spike
	{"progs/spike.mdl",				{0xebd9adaf, 0xfb3b2f28, 0x67cc2c34, 0x926ec21a, 0x09e1a233}},
	{"progs/spike.mdl",				{0xe78bb944, 0x92a753e4, 0x435c226b, 0x4021a65e, 0xef388c6b}},	// mdlhash_debug_spike
	{"progs/spike.mdl",				{0x28f1cb95, 0xafb8ed91, 0x6a8300ff, 0xeb29c03f, 0x28a2bbcb}},	// mdlhash_plaguespak_spike
	{"progs/spike.mdl",				{0xd8225980, 0x0299e9f7, 0x6732fd66, 0x54555264, 0x67bda403}},	// mdlhash_ruohis_spike
	{"progs/suit.mdl",				{0xb7dcb9dd, 0xed8da03b, 0x416efc5e, 0x8ee38d5a, 0x4063bf25}},
	{"progs/suit.mdl",				{0xbf4de4f3, 0xe60927c1, 0xf4824f00, 0x21feef56, 0xac00a897}},	// mdlhash_ruohis_suit
	{"progs/player.mdl",			{0x95ca0ab4, 0x021be62e, 0x6655e9a5, 0xd4a7ef1c, 0xb484582f}},
	{"progs/player.mdl",			{0xc0223459, 0x42b97d8d, 0xc52ff472, 0xf3ee0710, 0x41e21132}},	// mdlhash_player_mdl_CapNBubs_FMOD_DM
	{"progs/s_bubble.spr",			{0x3dbefaf2, 0x73b38654, 0x70910774, 0xdfe285f3, 0x4db5d90e}},
	{"progs/s_explod.spr",			{0x295a7b06, 0xacdf1f88, 0x50a20894, 0x5c75905f, 0xb9d95d0b}},
	{"maps/b_bh100.bsp",			{0x65e7e302, 0x94a85b4d, 0x8092e674, 0xf700e5f0, 0xde667fcc}},
	{"maps/b_bh100.bsp",			{0x527fd5b6, 0xd490707f, 0xabd69622, 0xf3d18a69, 0xe9bc0eb9}},	// mdlhash_ruohis_b_bh100
	{"maps/b_bh100.bsp",			{0xb42135c1, 0x6816429c, 0x21e3ce3e, 0xfab9f8fc, 0x09387ae1}},	// mdlhash_ruohis_b_bh100_other
	{"maps/b_bh100.bsp",			{0xc6b455ff, 0xad7cb845, 0xa7356729, 0x35375fcf, 0x10adb479}},	// mdlhash_generations_b_bh100
	{"sound/buttons/airbut1.wav",	{0x47d5141e, 0xc925e8eb, 0x26e5583c, 0xc8dfd021, 0x226792ef}},
	{"sound/buttons/airbut1.wav",	{0xd5fbbce0, 0x2083e431, 0xaa11c54d, 0x089ce053, 0xdf03ce11}},	// sound_buttons_mindgrid_airbut1
	{"sound/buttons/airbut1.wav",	{0x81911593, 0x3e85656b, 0x7a9475d0, 0x87de29a0, 0x157d3ef9}},	// sound_buttons_rerelease_airbut1
	{"sound/items/armor1.wav",		{0x44488db0, 0xef0b0a1d, 0x3acda8b4, 0x3d87b467, 0xe4dd4fcc}},
	{"sound/items/armor1.wav",		{0x1b47daed, 0x5a60cd6a, 0x83940499, 0x65eb651b, 0x4441f6cf}},	// sound_items_mindgrid_armor1
	{"sound/items/armor1.wav",		{0x1bde8fc5, 0xc979543f, 0x8fc7f57b, 0xe2c1367b, 0x62cac72f}},	// sound_items_rerelease_armor1
	{"sound/items/damage.wav",		{0x197778ac, 0x92a252c5, 0x90c30256, 0x7ba6f264, 0x56ab654f}},
	{"sound/items/damage.wav",		{0x3da087a3, 0x1799d4c7, 0x8ddabf31, 0x42e2de6f, 0x5bfdabec}},	// sound_items_mindgrid_damage
	{"sound/items/damage.wav",		{0x4dde0192, 0x1791d662, 0xa33168eb, 0xb98d083f, 0x15ba0ded}},	// sound_items_rerelease_damage
	{"sound/items/damage2.wav",		{0x719a9b4d, 0x1c76b218, 0xfcbf979f, 0x5ea5e6fa, 0x68afda0e}},
	{"sound/items/damage2.wav",		{0xd0eb779c, 0x0bf1d83c, 0x120e1c7e, 0x50fa449b, 0x2cd4655b}},	// sound_items_mindgrid_damage2
	{"sound/items/damage2.wav",		{0x6c9b6f7c, 0x85e48943, 0x803ddafd, 0x8249af31, 0x9967d89c}},	// sound_items_rerelease_damage2
	{"sound/items/damage3.wav",		{0xfa07de6c, 0x6e306d39, 0x7d18f3ed, 0x90272558, 0x63d01e7a}},
	{"sound/items/damage3.wav",		{0x2e0c3b71, 0xdd52cfc9, 0x8e5fe1e5, 0x0ac48fd9, 0xa2d8b970}},	// sound_items_mindgrid_damage3
	{"sound/items/damage3.wav",		{0x18a390ad, 0x0c47d3e9, 0x19db8c3b, 0x1426eda5, 0xa2feb55a}},	// sound_items_rerelease_damage3
	{"sound/items/health1.wav",		{0xebdd17b2, 0xa1289b80, 0x7810e3f2, 0xdb8d01bf, 0x0b495a96}},
	{"sound/items/health1.wav",		{0x3ef1d863, 0x683899cb, 0xd9191607, 0x02353423, 0x0737419c}},	// sound_items_rerelease_health1
	{"sound/items/inv1.wav",		{0xa01240b8, 0xb588d430, 0xc62406e0, 0x39e59dfd, 0xad5a4b98}},
	{"sound/items/inv1.wav",		{0x787708ba, 0x63f52c51, 0x55835932, 0x7db46d86, 0xccc830cc}},	// sound_items_mindgrid_inv1
	{"sound/items/inv1.wav",		{0x24c5cf56, 0xb3569f7a, 0xb21bb96a, 0x95638065, 0x2ea0d036}},	// sound_items_rerelease_inv1
	{"sound/items/inv2.wav",		{0x9ab6025d, 0x2d9d24a2, 0xc127b17c, 0x019e908a, 0xa521f7d3}},
	{"sound/items/inv2.wav",		{0xb2a3bc8c, 0x7eb56b5f, 0x675e4d67, 0x24054040, 0x3b2ecd0b}},	// sound_items_mindgrid_inv2
	{"sound/items/inv2.wav",		{0x810f09ea, 0x34d8c482, 0xcc9561b1, 0xffeba6d0, 0xc6610c57}},	// sound_items_rerelease_inv2
	{"sound/items/inv3.wav",		{0xfb785777, 0x9c622826, 0x6121d05c, 0xf42d5661, 0xa34a5429}},
	{"sound/items/inv3.wav",		{0xa1945433, 0x7f3e2c52, 0x9144528a, 0x02891cf0, 0xd95eb255}},	// sound_items_mindgrid_inv3
	{"sound/items/inv3.wav",		{0x037ed5b5, 0x27ab500b, 0xcee3abb1, 0xd882645a, 0x48e02c87}},	// sound_items_rerelease_inv3
	{"sound/items/itembk2.wav",		{0x178c519b, 0x3b030527, 0x759aaed2, 0xf7dca7d7, 0xf01a1e36}},
	{"sound/items/itembk2.wav",		{0x41dd6dad, 0x45a8c24b, 0xef56b75e, 0x5ab3915a, 0x2901a70f}},	// sound_items_mindgrid_itembk2
	{"sound/items/itembk2.wav",		{0xf3937197, 0x71d317b7, 0x7827aee9, 0x68ab6f3a, 0x5cb0e9c7}},	// sound_items_rerelease_itembk2
	{"sound/player/land.wav",		{0xb8bb5e41, 0xf087ad8e, 0x13323cd5, 0x2e2d2052, 0x338e9e38}},
	{"sound/player/land.wav",		{0x349bc931, 0x8621a826, 0xca259e9d, 0x5eb296d1, 0xe0f4ac70}},	// sound_player_rerelease_land
	{"sound/player/land2.wav",		{0x6c13b55c, 0xc46c1589, 0xeeabac42, 0x087cd24e, 0xd9552386}},
	{"sound/player/land2.wav",		{0x8ac31277, 0xdfc8eedf, 0xf6f322ca, 0x37188a1f, 0xe3f7a688}},	// sound_player_rerelease_land2
	{"sound/misc/outwater.wav",		{0x9cb6fc36, 0x9c20e9ba, 0x595f8418, 0x50e76d9f, 0xa7503dfd}},
	{"sound/misc/outwater.wav",		{0x6be98515, 0xfeab0126, 0x80edc811, 0xe7cf7012, 0x1def4980}},	// sound_misc_mindgrid_outwater
	{"sound/misc/outwater.wav",		{0x13930c6c, 0xaa57abbf, 0x5356d6ea, 0x1a2e6d40, 0x6a818b4c}},	// sound_misc_rerelease_outwater
	{"sound/weapons/pkup.wav",		{0x05a43523, 0x09bbab60, 0x77ce67a0, 0xb52fe23d, 0xf2715701}},
	{"sound/weapons/pkup.wav",		{0xda261fbc, 0x12f47c68, 0x9e91e794, 0xce0a434d, 0xc4e35e77}},	// sound_weapons_mindgrid_pkup
	{"sound/weapons/pkup.wav",		{0x6a0c9888, 0x2d68fa77, 0x5e448d95, 0x2a95666c, 0x842ee9a9}},	// sound_weapons_rerelease_pkup
	{"sound/player/plyrjmp8.wav",	{0x75e2e946, 0xc9ad54f2, 0x6cd47b03, 0xeef0c9d4, 0x5d3386da}},
	{"sound/player/plyrjmp8.wav",	{0x5a9e8263, 0xb9b3f06e, 0x8a047de1, 0x72c18bec, 0x547df977}},	// sound_player_mindgrid_plyrjmp8
	{"sound/player/plyrjmp8.wav",	{0xd33205fa, 0x2ec2aa75, 0x074e991b, 0x2f32c63e, 0xafbbbb6a}},	// sound_player_rerelease_plyrjmp8
	{"sound/items/protect.wav",		{0x38ff1351, 0x7d6799c7, 0x7577256d, 0xbf233192, 0xb6223baa}},
	{"sound/items/protect.wav",		{0x5948333f, 0xa28e882a, 0xb4b82964, 0xe10679c5, 0x191f8cb1}},	// sound_items_mindgrid_protect
	{"sound/items/protect.wav",		{0x7f894d4b, 0xe4a5c499, 0xd06df01a, 0xb1e57bc8, 0xb2d552aa}},	// sound_items_rerelease_protect
	{"sound/items/protect2.wav",	{0x588dd159, 0xae489f2c, 0x22f7118e, 0x487eeb4c, 0x3c54c997}},
	{"sound/items/protect2.wav",	{0x29e977b0, 0x0d9378fe, 0xb4489037, 0x02d83d62, 0xce0b718d}},	// sound_items_mindgrid_protect2
	{"sound/items/protect2.wav",	{0xa8a31b8a, 0xf96967b1, 0xc394287f, 0x3a5bf203, 0xe80d111b}},	// sound_items_rerelease_protect2
	{"sound/items/protect3.wav",	{0x869a1143, 0xc9e6a404, 0xcbd5d90e, 0xa1b57e4c, 0xae77dd20}},
	{"sound/items/protect3.wav",	{0x7619314c, 0x07b6901c, 0x000601e2, 0xc358367e, 0x999f90b1}},	// sound_items_mindgrid_protect3
	{"sound/items/protect3.wav",	{0xd8a88cdf, 0x394d1b5b, 0x28fb397b, 0x4120f605, 0x3e5dbefa}},	// sound_items_rerelease_protect3
	{"sound/items/r_item1.wav",		{0xdde12941, 0xfbb3b804, 0x776cf5fa, 0xe81df8ea, 0xa1429f63}},
	{"sound/items/r_item1.wav",		{0x0f5754d4, 0xa8b84c5f, 0x0001903a, 0xd3ab289c, 0xa93bc848}},	// sound_items_mindgrid_r_item1
	{"sound/items/r_item1.wav",		{0xc850a036, 0x86199e27, 0x8550ac67, 0x7fc29886, 0x1d80678a}},	// sound_items_rerelease_r_item1
	{"sound/items/r_item2.wav",		{0xd9da4047, 0x5a7b191a, 0xc02d860d, 0x18f679de, 0xc47bd93a}},
	{"sound/items/r_item2.wav",		{0xf62a00d2, 0x5fceaaca, 0x25f91692, 0xf7602cb7, 0x230da525}},	// sound_items_r_item2_wav_us
	{"sound/items/r_item2.wav",		{0x83a8e646, 0xbc4cc313, 0xa07aa96e, 0x9d81eada, 0xf619bd10}},	// sound_items_r_item2_wav_ru
	{"sound/items/r_item2.wav",		{0xd2d0de39, 0x2bb78d33, 0x09ef3081, 0xbe90bacf, 0xbf71ecce}},	// sound_items_mindgrid_r_item2
	{"sound/items/r_item2.wav",		{0xb0e2cb5d, 0xcc12db33, 0x5d599343, 0x2835ac62, 0x76fa4846}},	// sound_items_rerelease_r_item2
	{"sound/misc/water1.wav",		{0x8625dbc7, 0xbaf30ae6, 0xfdb53965, 0x2d7eb9d6, 0xf9fd9304}},
	{"sound/misc/water1.wav",		{0x838a0318, 0x0805e53e, 0x6ebf4065, 0xaa5fb9a0, 0xe1cad807}},	// sound_misc_mindgrid_water1
	{"sound/misc/water1.wav",		{0xa694280d, 0xd6b28324, 0x9e2c2def, 0x007d86f7, 0xbb583438}},	// sound_misc_rerelease_water1
	{"sound/misc/water2.wav",		{0xccda75ec, 0xfb5cd780, 0xe2d73b5b, 0x9f3560ad, 0x4e116b85}},
	{"sound/misc/water2.wav",		{0xfb0a6b17, 0x065fbc33, 0x5fab7cc5, 0x110c01aa, 0x278b612e}},	// sound_misc_mindgrid_water2
	{"sound/misc/water2.wav",		{0xd974d2db, 0x7c609ba1, 0x9ffefe98, 0x376e5f5d, 0x8cc0cbb9}},	// sound_misc_rerelease_water2
	{"sound/misc/menu1.wav",		{0x2e19b817, 0x0c0e1c5f, 0xd7a0ecf8, 0xb278c27e, 0xb0e1923c}},
	{"sound/misc/menu1.wav",		{0x0849e8f9, 0x5828515c, 0x55d6b38c, 0xf7299ac3, 0x0655378e}},	// sound_misc_rerelease_menu1
	{"sound/misc/menu2.wav",		{0xd6719ab1, 0x0a407e2b, 0xb03a053b, 0x2a4bc2cb, 0x61cd7ff3}},
	{"sound/misc/menu2.wav",		{0xe028d3c0, 0x710031e1, 0xd83221ad, 0xd28205c7, 0x51c10a10}},	// sound_misc_rerelease_menu2
	{"sound/misc/menu3.wav",		{0xae120b9a, 0x3a217d2e, 0xc34f0990, 0x2d4332ed, 0x97c18f76}},
	{"sound/misc/menu3.wav",		{0x2205061f, 0x00f4c078, 0xfa26934b, 0x15bacdfd, 0x98d90a7b}},	// sound_misc_rerelease_menu3
	{"sound/misc/talk.wav",			{0x5b15321c, 0x1af2d826, 0xb0228e9f, 0x49c06f56, 0xa8355e8b}},
	{"sound/misc/talk.wav",			{0xf28522df, 0x7ea37652, 0xc4605d1f, 0xce10e1cf, 0x2d1fc5fc}},	// sound_misc_rerelease_talk
	{"sound/misc/basekey.wav",		{0x926134ce, 0x22806dd3, 0x1952624a, 0x8f43f7e9, 0xa6fdfe64}},
	{"sound/misc/basekey.wav",		{0xc99871d4, 0xc60e0fef, 0x14e64bf9, 0xbaf43934, 0x5376df18}},	// sound_gpl_maps_silence_wav
	{"sound/misc/basekey.wav",		{0x6871924c, 0xbcc9e6dc, 0x6cea01c2, 0x367f979f, 0xb16619ae}},	// sound_misc_rerelease_basekey
	{"sound/doors/runeuse.wav",		{0xf41d6d59, 0xe99b93e8, 0x15edfd25, 0x668a499c, 0x1ac62572}},
	{"sound/doors/runeuse.wav",		{0x00ffef66, 0x4ed15aae, 0x8e67b5ef, 0x563f0c0f, 0x70b11c41}},	// sound_doors_rerelease_runeuse
	{"gfx/colormap.lmp",			{0xa93b3795, 0x17016397, 0x0a761d38, 0x4866e67d, 0x3c2a2a75}},
	{"gfx/palette.lmp",				{0xa6a2e242, 0xbad0f7da, 0xe263351f, 0x6d51f8ad, 0x49a45d4a}},
};

#define NUM_HASHES	(sizeof(fmod_hashes) / sizeof(fmod_hashes[0]))
#define MAX_FILES	NUM_HASHES

typedef struct
{
	const char	*name;
	bool		checked;		// loaded since the client started
	bool		modified;		// and not one of those allowed
} fmod_file_t;

static fmod_file_t	fmod_files[MAX_FILES];
static int			fmod_numfiles;
static double		fmod_warntime;

/*
================
CL_FModLoaded

A file as it is loaded: if it is one checked, whether it is allowed. One
that was and now isn't (loaded again, changed) is said.
================
*/
static void CL_FModLoaded (const char *path, const byte *data, int length)
{
	fmod_file_t	*file;
	byte		digest[SHA1_DIGEST_SIZE];
	uint32_t	words[5];
	bool		modified = true;
	size_t		i;
	int			f, w;

	for (f = 0 ; f < fmod_numfiles && Q_strcasecmp (path, fmod_files[f].name) ; f++)
		;
	if (f == fmod_numfiles)
		return;
	file = &fmod_files[f];

	SHA1_Block (data, (size_t)length, digest);
	for (w = 0 ; w < 5 ; w++)
		words[w] = (uint32_t)digest[w*4] | (uint32_t)digest[w*4+1] << 8 | (uint32_t)digest[w*4+2] << 16
			| (uint32_t)digest[w*4+3] << 24;
	for (i = 0 ; i < NUM_HASHES && modified ; i++)
		if (!Q_strcasecmp (fmod_hashes[i].name, file->name) && !memcmp (fmod_hashes[i].hash, words, sizeof(words)))
			modified = false;

	if (file->checked && !file->modified && modified && cls.state == ca_active
		&& (!fmod_warntime || host.realtime - fmod_warntime >= 3))
	{
		Cbuf_AddText ("say warning: models changed !!  Use f_modified again\n");
		fmod_warntime = host.realtime;
	}
	file->checked = true;
	file->modified = modified;
}

/*
================
CL_FModText

"all models ok", or "modified:" and the files that aren't
================
*/
const char *CL_FModText (void)
{
	static char	text[512];
	int			f, count = 0;

	Q_strncpyz (text, "modified:", sizeof(text));
	for (f = 0 ; f < fmod_numfiles ; f++)
	{
		if (!fmod_files[f].checked || !fmod_files[f].modified)
			continue;
		if (strlen (text) >= 240)
		{
			Q_strncatz (text, " & more...", sizeof(text));
			break;
		}
		Q_strncatz (text, " ", sizeof(text));
		Q_strncatz (text, strrchr (fmod_files[f].name, '/') + 1, sizeof(text));
		count++;
	}
	if (!count)
		Q_strncpyz (text, "all models ok", sizeof(text));
	return text;
}

// said to the server, in green or red; printed when not connected
void CL_FModResponse (void)
{
	const char	*text = CL_FModText ();
	const char	*color = strcmp (text, "all models ok") ? "&cf00" : "&c0f0";

	if (cls.state == ca_disconnected || cls.demoplayback)
		Con_Printf ("%s%s&r\n", color, text);
	else
		Cbuf_AddText (va ("say {%s%s&r}\n", color, text));
}

void CL_InitFMod (void)
{
	size_t	i;
	int		f;

	// each file once, in the order of the hashes
	for (i = 0 ; i < NUM_HASHES ; i++)
	{
		for (f = 0 ; f < fmod_numfiles && strcmp (fmod_files[f].name, fmod_hashes[i].name) ; f++)
			;
		if (f == fmod_numfiles)
			fmod_files[fmod_numfiles++].name = fmod_hashes[i].name;
	}
	FS_SetLoadHook (CL_FModLoaded);
	Cmd_AddCommand ("f_modified", CL_FModResponse,
		"Says in chat which checked models, sounds and palette files are modified, or \"all models ok\"; "
		"prints it offline or in a demo.");
}
