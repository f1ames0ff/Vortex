import * as path from "path";

import format from "string-template";

import getVortexPath from "../../util/getVortexPath";
import { protonMyGamesPath } from "../../util/linux/proton";
import { makeOverlayableDictionary } from "../../util/util";
import type { IDiscoveryResult } from "../gamemode_management/types/IDiscoveryResult";
import { getGame } from "../gamemode_management/util/getGame";

interface IGameSupport {
  iniFiles: string[];
  iniFormat: string;
}

const gameSupport = makeOverlayableDictionary<string, IGameSupport>(
  {
    skyrim: {
      iniFiles: [
        path.join("{mygames}", "Skyrim", "Skyrim.ini"),
        path.join("{mygames}", "Skyrim", "SkyrimPrefs.ini"),
      ],
      iniFormat: "winapi",
    },
    enderal: {
      iniFiles: [
        path.join("{mygames}", "Enderal", "Enderal.ini"),
        path.join("{mygames}", "Enderal", "EnderalPrefs.ini"),
      ],
      iniFormat: "winapi",
    },
    skyrimse: {
      iniFiles: [
        path.join("{mygames}", "Skyrim Special Edition", "Skyrim.ini"),
        path.join("{mygames}", "Skyrim Special Edition", "SkyrimPrefs.ini"),
        path.join("{mygames}", "Skyrim Special Edition", "SkyrimCustom.ini"),
      ],
      iniFormat: "winapi",
    },
    enderalspecialedition: {
      iniFiles: [
        path.join("{mygames}", "Enderal Special Edition", "Enderal.ini"),
        path.join("{mygames}", "Enderal Special Edition", "EnderalPrefs.ini"),
      ],
      iniFormat: "winapi",
    },
    skyrimvr: {
      iniFiles: [
        path.join("{mygames}", "Skyrim VR", "Skyrim.ini"),
        path.join("{mygames}", "Skyrim VR", "SkyrimVR.ini"),
        path.join("{mygames}", "Skyrim VR", "SkyrimPrefs.ini"),
      ],
      iniFormat: "winapi",
    },
    fallout3: {
      iniFiles: [
        path.join("{mygames}", "Fallout3", "Fallout.ini"),
        path.join("{mygames}", "Fallout3", "FalloutPrefs.ini"),
        path.join("{mygames}", "Fallout3", "FalloutCustom.ini"),
      ],
      iniFormat: "winapi",
    },
    fallout4: {
      iniFiles: [
        path.join("{mygames}", "Fallout4", "Fallout4.ini"),
        path.join("{mygames}", "Fallout4", "Fallout4Prefs.ini"),
        path.join("{mygames}", "Fallout4", "Fallout4Custom.ini"),
      ],
      iniFormat: "winapi",
    },
    fallout4vr: {
      iniFiles: [
        path.join("{mygames}", "Fallout4VR", "Fallout4Custom.ini"),
        path.join("{mygames}", "Fallout4VR", "Fallout4Prefs.ini"),
      ],
      iniFormat: "winapi",
    },
    falloutnv: {
      iniFiles: [
        path.join("{mygames}", "FalloutNV", "Fallout.ini"),
        path.join("{mygames}", "FalloutNV", "FalloutPrefs.ini"),
        path.join("{mygames}", "FalloutNV", "FalloutCustom.ini"),
      ],
      iniFormat: "winapi",
    },
    starfield: {
      iniFiles: [
        path.join("{mygames}", "Starfield", "Starfield.ini"),
        path.join("{mygames}", "Starfield", "StarfieldPrefs.ini"),
        path.join("{mygames}", "Starfield", "StarfieldCustom.ini"),
      ],
      iniFormat: "winapi",
    },
    oblivion: {
      iniFiles: [path.join("{mygames}", "Oblivion", "Oblivion.ini")],
      iniFormat: "winapi",
    },
    oblivionremastered: {
      iniFiles: [
        path.join("{mygames}", "Oblivion Remastered", "Saved", "Config", "Windows", "Altar.ini"),
      ],
      iniFormat: "winapi",
    },
    morrowind: {
      iniFiles: [path.join("{game}", "Morrowind.ini")],
      iniFormat: "winapi",
    },
  },
  {
    gog: {
      skyrimse: {
        iniFiles: [
          path.join("{mygames}", "Skyrim Special Edition GOG", "Skyrim.ini"),
          path.join("{mygames}", "Skyrim Special Edition GOG", "SkyrimPrefs.ini"),
          path.join("{mygames}", "Skyrim Special Edition GOG", "SkyrimCustom.ini"),
        ],
        iniFormat: "winapi",
      },
      enderalspecialedition: {
        iniFiles: [
          path.join("{mygames}", "Enderal Special Edition GOG", "Enderal.ini"),
          path.join("{mygames}", "Enderal Special Edition GOG", "EnderalPrefs.ini"),
        ],
        iniFormat: "winapi",
      },
    },
    epic: {
      skyrimse: {
        iniFiles: [
          path.join("{mygames}", "Skyrim Special Edition EPIC", "Skyrim.ini"),
          path.join("{mygames}", "Skyrim Special Edition EPIC", "SkyrimPrefs.ini"),
          path.join("{mygames}", "Skyrim Special Edition EPIC", "SkyrimCustom.ini"),
        ],
        iniFormat: "winapi",
      },
      fallout4: {
        iniFiles: [
          path.join("{mygames}", "Fallout4 EPIC", "Fallout4.ini"),
          path.join("{mygames}", "Fallout4 EPIC", "Fallout4Prefs.ini"),
          path.join("{mygames}", "Fallout4 EPIC", "Fallout4Custom.ini"),
        ],
        iniFormat: "winapi",
      },
    },
    xbox: {
      skyrimse: {
        iniFiles: [
          path.join("{mygames}", "Skyrim Special Edition MS", "Skyrim.ini"),
          path.join("{mygames}", "Skyrim Special Edition MS", "SkyrimPrefs.ini"),
          path.join("{mygames}", "Skyrim Special Edition MS", "SkyrimCustom.ini"),
        ],
        iniFormat: "winapi",
      },
      fallout4: {
        iniFiles: [
          path.join("{mygames}", "Fallout4 MS", "Fallout4.ini"),
          path.join("{mygames}", "Fallout4 MS", "Fallout4Prefs.ini"),
          path.join("{mygames}", "Fallout4 MS", "Fallout4Custom.ini"),
        ],
        iniFormat: "winapi",
      },
    },
    enderalOverride: {
      enderalspecialedition: {
        iniFiles: [
          path.join("{mygames}", "Skyrim Special Edition", "Skyrim.ini"),
          path.join("{mygames}", "Skyrim Special Edition", "SkyrimPrefs.ini"),
          path.join("{mygames}", "Skyrim Special Edition", "SkyrimCustom.ini"),
        ],
        iniFormat: "winapi",
      },
    },
  },
  (gameId: string, store: string) => store,
);

export function iniFiles(gameMode: string, discovery: IDiscoveryResult) {
  let store = discovery?.store;

  // override for the case where enderal se is installed as a total conversion
  // instead of the stand-alone version
  if (
    gameMode === "enderalspecialedition" &&
    discovery?.path !== undefined &&
    discovery?.path.toLowerCase().includes("skyrim")
  ) {
    store = "enderaloverride";
  }

  const templates = gameSupport.get(gameMode, "iniFiles", store) ?? [];
  // All templates start with {mygames}/<game folder>/<file>; use that folder
  // to find the game's My Games directory inside a Proton prefix.
  const marker = "{mygames}";
  const firstTemplate = templates[0];
  const gameFolder =
    firstTemplate !== undefined && firstTemplate.startsWith(marker)
      ? firstTemplate.slice(marker.length).split(/[/\\]/).filter(Boolean)[0]
      : undefined;
  const markerFile = firstTemplate !== undefined ? path.basename(firstTemplate) : undefined;
  const steamAppId = getGame(gameMode)?.details?.steamAppId;
  const mygames =
    (gameFolder !== undefined
      ? protonMyGamesPath(discovery?.path, gameFolder, markerFile, steamAppId)
      : undefined) ?? path.join(getVortexPath("documents"), "My Games");

  return templates.map((filePath) => format(filePath, { mygames, game: discovery.path }));
}

export function iniFormat(gameMode: string) {
  return gameSupport.get(gameMode, "iniFormat");
}
