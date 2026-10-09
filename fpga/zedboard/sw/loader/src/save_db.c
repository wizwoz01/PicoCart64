/**
 * SPDX-License-Identifier: BSD-2-Clause AND ISC
 *
 * Save type of a game, from the ID in its ROM header.
 *
 * The table and the special cases come from the save type heuristics of ares
 * (mia/medium/nintendo-64.cpp):
 *
 *   Copyright (c) 2004-2025 ares team, Near et al
 *
 *   Permission to use, copy, modify, and/or distribute this software for any
 *   purpose with or without fee is hereby granted, provided that the above
 *   copyright notice and this permission notice appear in all copies.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *   WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *   ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stddef.h>
#include <string.h>

#include "save_db.h"

// Media type + 2-character game ID (header bytes 0x3B-0x3D)
static const struct {
	char id[4];
	save_type_t type;
} games[] = {
	{ "CDZ", SAVE_SRAM_96K },	// Dezaemon 3D
	{ "CFZ", SAVE_SRAM },	// F-Zero X (J)
	{ "CLB", SAVE_EEPROM_4K },	// Mario Party (NTSC)
	{ "CP2", SAVE_FLASHRAM },	// Pocket Monsters Stadium 2 (J)
	{ "CPS", SAVE_SRAM },	// Pocket Monsters Stadium (J)
	{ "CZL", SAVE_SRAM },	// Legend of Zelda: Ocarina of Time [Zelda no Densetsu - Toki no Ocarina (J)]
	{ "N3D", SAVE_EEPROM_16K },	// Doraemon 3: Nobita no Machi SOS!
	{ "NA2", SAVE_SRAM },	// Virtual Pro Wrestling 2
	{ "NAB", SAVE_EEPROM_4K },	// Air Boarder 64
	{ "NAD", SAVE_EEPROM_4K },	// Worms Armageddon (U)
	{ "NAF", SAVE_FLASHRAM },	// Doubutsu no Mori
	{ "NAG", SAVE_EEPROM_4K },	// AeroGauge
	{ "NAL", SAVE_SRAM },	// Super Smash Bros. [Nintendo All-Star! Dairantou Smash Brothers (J)]
	{ "NB5", SAVE_SRAM },	// Biohazard 2 (J)
	{ "NB6", SAVE_EEPROM_4K },	// Super B-Daman: Battle Phoenix 64
	{ "NB7", SAVE_EEPROM_16K },	// Banjo-Tooie [Banjo to Kazooie no Daiboken 2 (J)]
	{ "NBC", SAVE_EEPROM_4K },	// Blast Corps
	{ "NBD", SAVE_EEPROM_4K },	// Bomberman Hero [Mirian Ojo o Sukue! (J)]
	{ "NBH", SAVE_EEPROM_4K },	// Body Harvest
	{ "NBK", SAVE_EEPROM_4K },	// Banjo-Kazooie [Banjo to Kazooie no Daiboken (J)]
	{ "NBM", SAVE_EEPROM_4K },	// Bomberman 64 [Baku Bomberman (J)]
	{ "NBN", SAVE_EEPROM_4K },	// Bakuretsu Muteki Bangaioh
	{ "NBV", SAVE_EEPROM_4K },	// Bomberman 64: The Second Attack! [Baku Bomberman 2 (J)]
	{ "NCC", SAVE_FLASHRAM },	// Command & Conquer
	{ "NCG", SAVE_EEPROM_4K },	// Choro Q 64 II - Hacha Mecha Grand Prix Race (J)
	{ "NCH", SAVE_EEPROM_4K },	// Chopper Attack
	{ "NCK", SAVE_FLASHRAM },	// NBA Courtside 2 featuring Kobe Bryant
	{ "NCR", SAVE_EEPROM_4K },	// Penny Racers [Choro Q 64 (J)]
	{ "NCT", SAVE_EEPROM_4K },	// Chameleon Twist
	{ "NCU", SAVE_EEPROM_4K },	// Cruis'n USA
	{ "NCV", SAVE_FLASHRAM },	// Cubivore [English translation/patch of Doubutsu Banchou (J)]
	{ "NCW", SAVE_EEPROM_16K },	// Cruis'n World
	{ "NCX", SAVE_EEPROM_4K },	// Custom Robo
	{ "NCZ", SAVE_EEPROM_16K },	// Custom Robo V2
	{ "ND2", SAVE_EEPROM_16K },	// Doraemon 2: Nobita to Hikari no Shinden
	{ "ND6", SAVE_EEPROM_16K },	// Densha de Go! 64
	{ "NDA", SAVE_FLASHRAM },	// Derby Stallion 64
	{ "NDO", SAVE_EEPROM_16K },	// Donkey Kong 64
	{ "NDP", SAVE_FLASHRAM },	// Dinosaur Planet (Unlicensed)
	{ "NDQ", SAVE_EEPROM_4K },	// Disney's Donald Duck - Goin' Quackers [Quack Attack (E)]
	{ "NDR", SAVE_EEPROM_4K },	// Doraemon: Nobita to 3tsu no Seireiseki
	{ "NDU", SAVE_EEPROM_4K },	// Duck Dodgers starring Daffy Duck
	{ "NDY", SAVE_EEPROM_4K },	// Diddy Kong Racing
	{ "NEA", SAVE_EEPROM_4K },	// PGA European Tour
	{ "NEP", SAVE_EEPROM_16K },	// Star Wars Episode I: Racer
	{ "NER", SAVE_EEPROM_4K },	// Aero Fighters Assault [Sonic Wings Assault (J)]
	{ "NEV", SAVE_EEPROM_16K },	// Neon Genesis Evangelion
	{ "NF2", SAVE_EEPROM_4K },	// F-1 World Grand Prix II
	{ "NFG", SAVE_EEPROM_4K },	// Fighter Destiny 2
	{ "NFH", SAVE_EEPROM_4K },	// In-Fisherman Bass Hunter 64
	{ "NFU", SAVE_EEPROM_16K },	// Conker's Bad Fur Day
	{ "NFW", SAVE_EEPROM_4K },	// F-1 World Grand Prix
	{ "NFX", SAVE_EEPROM_4K },	// Star Fox 64 [Lylat Wars (E)]
	{ "NFY", SAVE_EEPROM_4K },	// Kakutou Denshou: F-Cup Maniax
	{ "NFZ", SAVE_SRAM },	// F-Zero X (U + E)
	{ "NG6", SAVE_SRAM },	// Ganmare Goemon: Dero Dero Douchuu Obake Tenkomori
	{ "NGC", SAVE_EEPROM_16K },	// GT 64: Championship Edition
	{ "NGE", SAVE_EEPROM_4K },	// GoldenEye 007
	{ "NGL", SAVE_EEPROM_4K },	// Getter Love!!
	{ "NGP", SAVE_SRAM },	// Goemon: Mononoke Sugoroku
	{ "NGT", SAVE_EEPROM_16K },	// City Tour GrandPrix - Zen Nihon GT Senshuken
	{ "NGU", SAVE_EEPROM_4K },	// Tsumi to Batsu: Hoshi no Keishousha (Sin and Punishment)
	{ "NGV", SAVE_EEPROM_4K },	// Glover
	{ "NHA", SAVE_EEPROM_4K },	// Bomberman 64: Arcade Edition (J)
	{ "NHF", SAVE_EEPROM_4K },	// 64 Hanafuda: Tenshi no Yakusoku
	{ "NHP", SAVE_EEPROM_4K },	// Heiwa Pachinko World 64
	{ "NHY", SAVE_SRAM },	// Hybrid Heaven (J)
	{ "NIB", SAVE_SRAM },	// Itoi Shigesato no Bass Tsuri No. 1 Kettei Ban!
	{ "NIC", SAVE_EEPROM_4K },	// Indy Racing 2000
	{ "NIJ", SAVE_EEPROM_4K },	// Indiana Jones and the Infernal Machine
	{ "NIM", SAVE_EEPROM_16K },	// Ide Yosuke no Mahjong Juku
	{ "NIR", SAVE_EEPROM_4K },	// Utchan Nanchan no Hono no Challenger: Denryuu Ira Ira Bou
	{ "NJ5", SAVE_SRAM },	// Jikkyou Powerful Pro Yakyuu 5
	{ "NJF", SAVE_FLASHRAM },	// Jet Force Gemini [Star Twins (J)]
	{ "NJG", SAVE_SRAM },	// Jinsei Game 64
	{ "NJK", SAVE_EEPROM_4K },	// Viewpoint 2064 (Master)
	{ "NJM", SAVE_EEPROM_4K },	// Earthworm Jim 3D
	{ "NK2", SAVE_EEPROM_4K },	// Snowboard Kids 2 [Chou Snobow Kids (J)]
	{ "NKA", SAVE_EEPROM_4K },	// Fighters Destiny [Fighting Cup (J)]
	{ "NKG", SAVE_SRAM },	// Major League Baseball featuring Ken Griffey Jr.
	{ "NKI", SAVE_EEPROM_4K },	// Killer Instinct Gold
	{ "NKJ", SAVE_FLASHRAM },	// Ken Griffey Jr.'s Slugfest
	{ "NKT", SAVE_EEPROM_4K },	// Mario Kart 64
	{ "NLB", SAVE_EEPROM_4K },	// Mario Party (PAL)
	{ "NLL", SAVE_EEPROM_4K },	// Last Legion UX
	{ "NLR", SAVE_EEPROM_4K },	// Lode Runner 3-D
	{ "NM6", SAVE_FLASHRAM },	// Mega Man 64
	{ "NM8", SAVE_EEPROM_16K },	// Mario Tennis
	{ "NMF", SAVE_SRAM },	// Mario Golf 64
	{ "NMG", SAVE_EEPROM_4K },	// Monaco Grand Prix [Racing Simulation 2 (G)]
	{ "NMI", SAVE_EEPROM_4K },	// Mission: Impossible
	{ "NML", SAVE_EEPROM_4K },	// Mickey's Speedway USA [Mickey no Racing Challenge USA (J)]
	{ "NMO", SAVE_EEPROM_4K },	// Monopoly
	{ "NMQ", SAVE_FLASHRAM },	// Paper Mario
	{ "NMR", SAVE_EEPROM_4K },	// Multi-Racing Championship
	{ "NMS", SAVE_EEPROM_4K },	// Morita Shougi 64
	{ "NMU", SAVE_EEPROM_4K },	// Big Mountain 2000
	{ "NMV", SAVE_EEPROM_16K },	// Mario Party 3
	{ "NMW", SAVE_EEPROM_4K },	// Mario Party 2
	{ "NMX", SAVE_EEPROM_16K },	// Excitebike 64
	{ "NMZ", SAVE_EEPROM_4K },	// Zool - Majou Tsukai Densetsu (J)
	{ "NN6", SAVE_EEPROM_4K },	// Dr. Mario 64
	{ "NNA", SAVE_EEPROM_4K },	// Star Wars Episode I: Battle for Naboo
	{ "NNB", SAVE_EEPROM_16K },	// Kobe Bryant in NBA Courtside
	{ "NOB", SAVE_SRAM },	// Ogre Battle 64: Person of Lordly Caliber
	{ "NOS", SAVE_EEPROM_4K },	// 64 Oozumou
	{ "NP2", SAVE_EEPROM_4K },	// Chou Kuukan Night Pro Yakyuu King 2 (J)
	{ "NP3", SAVE_FLASHRAM },	// Pokemon Stadium 2 [Pocket Monsters Stadium - Kin Gin (J)]
	{ "NP4", SAVE_SRAM },	// Jikkyou Powerful Pro Yakyuu 4
	{ "NP6", SAVE_SRAM },	// Jikkyou Powerful Pro Yakyuu 6
	{ "NPA", SAVE_SRAM },	// Jikkyou Powerful Pro Yakyuu 2000
	{ "NPD", SAVE_EEPROM_16K },	// Perfect Dark
	{ "NPE", SAVE_SRAM },	// Jikkyou Powerful Pro Yakyuu Basic Ban 2001
	{ "NPF", SAVE_FLASHRAM },	// Pokemon Snap [Pocket Monsters Snap (J)]
	{ "NPG", SAVE_EEPROM_4K },	// Hey You, Pikachu! [Pikachu Genki Dechu (J)]
	{ "NPM", SAVE_SRAM },	// Premier Manager 64
	{ "NPN", SAVE_FLASHRAM },	// Pokemon Puzzle League
	{ "NPO", SAVE_FLASHRAM },	// Pokemon Stadium
	{ "NPP", SAVE_EEPROM_16K },	// Parlor! Pro 64: Pachinko Jikki Simulation Game
	{ "NPS", SAVE_SRAM },	// Jikkyou J.League 1999: Perfect Striker 2
	{ "NPT", SAVE_EEPROM_4K },	// Puyo Puyon Party
	{ "NPW", SAVE_EEPROM_4K },	// Pilotwings 64
	{ "NPY", SAVE_EEPROM_4K },	// Puyo Puyo Sun 64
	{ "NR7", SAVE_EEPROM_16K },	// Robot Poncots 64: 7tsu no Umi no Caramel
	{ "NRA", SAVE_EEPROM_4K },	// Rally '99 (J)
	{ "NRC", SAVE_EEPROM_4K },	// Top Gear Overdrive
	{ "NRE", SAVE_SRAM },	// Resident Evil 2
	{ "NRH", SAVE_FLASHRAM },	// Rockman Dash - Hagane no Boukenshin (J)
	{ "NRI", SAVE_SRAM },	// New Tetris, The
	{ "NRS", SAVE_EEPROM_4K },	// Star Wars: Rogue Squadron [Shutsugeki! Rogue Chuutai (J)]
	{ "NRZ", SAVE_EEPROM_16K },	// Ridge Racer 64
	{ "NS3", SAVE_EEPROM_4K },	// AI Shougi 3
	{ "NS4", SAVE_SRAM },	// Super Robot Taisen 64
	{ "NS6", SAVE_EEPROM_4K },	// Star Soldier: Vanishing Earth
	{ "NSA", SAVE_EEPROM_4K },	// Sonic Wings Assault (J)
	{ "NSC", SAVE_EEPROM_4K },	// Starshot: Space Circus Fever
	{ "NSI", SAVE_SRAM },	// Fushigi no Dungeon: Fuurai no Shiren 2
	{ "NSN", SAVE_EEPROM_4K },	// Snow Speeder (J)
	{ "NSQ", SAVE_FLASHRAM },	// StarCraft 64
	{ "NSS", SAVE_EEPROM_4K },	// Super Robot Spirits
	{ "NSU", SAVE_EEPROM_4K },	// Rocket: Robot on Wheels
	{ "NSV", SAVE_EEPROM_4K },	// Space Station Silicon Valley
	{ "NSW", SAVE_EEPROM_4K },	// Star Wars: Shadows of the Empire [Teikoku no Kage (J)]
	{ "NT3", SAVE_SRAM },	// Shin Nihon Pro Wrestling - Toukon Road 2 - The Next Generation (J)
	{ "NT6", SAVE_EEPROM_4K },	// Tetris 64
	{ "NT9", SAVE_FLASHRAM },	// Tigger's Honey Hunt
	{ "NTB", SAVE_EEPROM_4K },	// Transformers: Beast Wars Metals 64 (J)
	{ "NTC", SAVE_EEPROM_4K },	// 64 Trump Collection
	{ "NTE", SAVE_SRAM },	// 1080 Snowboarding
	{ "NTJ", SAVE_EEPROM_4K },	// Tom & Jerry in Fists of Fury
	{ "NTM", SAVE_EEPROM_4K },	// Mischief Makers [Yuke Yuke!! Trouble Makers (J)]
	{ "NTN", SAVE_EEPROM_4K },	// All Star Tennis '99
	{ "NTP", SAVE_EEPROM_4K },	// Tetrisphere
	{ "NTR", SAVE_EEPROM_4K },	// Top Gear Rally (J + E)
	{ "NTW", SAVE_EEPROM_4K },	// 64 de Hakken!! Tamagotchi
	{ "NTX", SAVE_EEPROM_4K },	// Taz Express
	{ "NUB", SAVE_EEPROM_16K },	// PD Ultraman Battle Collection 64
	{ "NUM", SAVE_SRAM },	// Nushi Zuri 64: Shiokaze ni Notte
	{ "NUT", SAVE_SRAM },	// Nushi Zuri 64
	{ "NVB", SAVE_SRAM },	// Bass Rush - ECOGEAR PowerWorm Championship (J)
	{ "NVL", SAVE_EEPROM_4K },	// V-Rally Edition '99
	{ "NVP", SAVE_SRAM },	// Virtual Pro Wrestling 64
	{ "NVY", SAVE_EEPROM_4K },	// V-Rally Edition '99 (J)
	{ "NW2", SAVE_SRAM },	// WCW-nWo Revenge
	{ "NW4", SAVE_FLASHRAM },	// WWF No Mercy
	{ "NWC", SAVE_EEPROM_4K },	// Wild Choppers
	{ "NWL", SAVE_SRAM },	// Waialae Country Club: True Golf Classics
	{ "NWQ", SAVE_EEPROM_4K },	// Rally Challenge 2000
	{ "NWU", SAVE_EEPROM_4K },	// Worms Armageddon (E)
	{ "NWX", SAVE_SRAM },	// WWF WrestleMania 2000
	{ "NXO", SAVE_EEPROM_4K },	// Cruis'n Exotica
	{ "NYK", SAVE_EEPROM_4K },	// Yakouchuu II: Satsujin Kouro
	{ "NYS", SAVE_EEPROM_16K },	// Yoshi's Story
	{ "NYW", SAVE_SRAM },	// Harvest Moon 64
	{ "NZL", SAVE_SRAM },	// Legend of Zelda: Ocarina of Time (E)
	{ "NZS", SAVE_FLASHRAM },	// Legend of Zelda: Majora's Mask [Zelda no Densetsu - Mujura no Kamen (J)]
};

save_type_t save_type_detect(const uint8_t *z64)
{
	const char id[4] = { (char)z64[0x3B], (char)z64[0x3C], (char)z64[0x3D], '\0' };
	const char region = (char)z64[0x3E];
	const uint8_t revision = z64[0x3F];
	size_t i;

	// Saves that depend on the region or the revision
	if (strcmp(id, "NSM") == 0 || strcmp(id, "NWR") == 0)
		return SAVE_EEPROM_4K;	// Super Mario 64, Wave Race 64 (also Shindou)
	if (strcmp(id, "NK4") == 0)	// Kirby 64: early Japanese releases use SRAM
		return (region == 'J' && revision < 2) ? SAVE_SRAM : SAVE_EEPROM_16K;
	if (strcmp(id, "N3H") == 0)	// Ganbare! Nippon! Olympics 2000 / International Track & Field 2000
		return region == 'J' ? SAVE_SRAM : SAVE_NONE;
	if (strcmp(id, "ND3") == 0 || strcmp(id, "ND4") == 0)	// Castlevania, Legacy of Darkness
		return region == 'J' ? SAVE_EEPROM_16K : SAVE_NONE;
	if (strcmp(id, "NDK") == 0 || strcmp(id, "NWT") == 0)	// Dark Rift, Wetrix
		return region == 'J' ? SAVE_EEPROM_4K : SAVE_NONE;

	for (i = 0; i < sizeof(games) / sizeof(games[0]); i++)
		if (memcmp(games[i].id, id, 3) == 0)
			return games[i].type;
	return SAVE_UNKNOWN;
}

const char *save_type_name(save_type_t type)
{
	switch (type) {
	case SAVE_NONE:
		return "none";
	case SAVE_EEPROM_4K:
		return "EEPROM 4 Kbit";
	case SAVE_EEPROM_16K:
		return "EEPROM 16 Kbit";
	case SAVE_SRAM:
		return "SRAM 256 Kbit";
	case SAVE_SRAM_96K:
		return "SRAM 768 Kbit";
	case SAVE_FLASHRAM:
		return "FlashRAM 1 Mbit";
	default:
		return "unknown";
	}
}
