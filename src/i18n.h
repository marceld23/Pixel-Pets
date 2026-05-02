#pragma once
#include <stdint.h>

// String IDs for the localization table. New entries are appended as the
// migration progresses through each module — phase 2 covers splash,
// settings, animal-select, bedtime, lockout. Later phases will fill in
// pickers, mini-games, help pages etc.
enum class Str : uint16_t {
  // Splash
  AppTagline,                 // "dein virtuelles Pet" / "your virtual Pet"
  SplashCredit,               // "von Justus und Marcel" / "by Justus and Marcel"

  // Settings
  SettingsTitle,
  SettingsVolume,
  SettingsBrightness,
  SettingsTimePrefix,         // "Uhrzeit:" / "Time:" + " HH:MM"
  SettingsTimeUnset,          // "Uhrzeit stellen" / "Set time"
  SettingsAnimalPrefix,       // "Tier:" / "Pet:"
  SettingsLangPrefix,         // "Sprache:" / "Language:"
  SettingsHelp,
  SettingsCredits,
  SettingsReset,
  SettingsResetConfirmTitle,  // "Wirklich zuruecksetzen?" / "Really reset?"
  SettingsResetConfirmBody,   // "Alle Daten gehen verloren." / "All data will be lost."
  SettingsResetConfirmYes,    // "Ja, loeschen" / "Yes, delete"
  SettingsResetConfirmNo,     // "Abbrechen" / "Cancel"
  SettingsWifi,               // "WLAN einrichten" / "WiFi setup"
  SettingsParentsHelp,        // "Eltern-Hilfe" / "For parents"
  SettingsWifiReset,          // "WLAN zuruecksetzen" / "Reset WiFi"
  WifiResetConfirmTitle,      // "WLAN-Daten loeschen?" / "Delete WiFi data?"
  SettingsParentSrv,          // "Eltern-Server" / "Parent server"
  SettingsParentSrvOff,       // "Aus" / "Off"
  SettingsParentSrvOn,        // "An" / "On"
  SettingsParentSrvConn,      // "Verbinde..." / "Connecting..."
  SettingsParentSrvFail,      // "Fehler" / "Failed"
  SettingsParentSrvHint,      // "Akku!" / "Battery!"
  BootCheckingWifi,           // "Checke WiFi" / "Checking WiFi"
  BootSyncingTime,            // "Synchronisiere Zeit" / "Syncing time"
  BootFetchingWorld,          // "Hole Wetter und Standort" / "Fetching weather + location"
  BootGreetingFrom,           // "Hallo aus" / "Hello from"
  SettingsLocation,           // "Standort" / "Location"
  LocationTitle,              // "Standort" / "Location"
  LocationCity,               // "Stadt:" / "City:"
  LocationCountry,            // "Land:" / "Country:"
  LocationCoords,             // "Koordinaten:" / "Coordinates:"
  LocationLastSeen,           // "Aktualisiert:" / "Last update:"
  LocationRefreshBtn,         // "Neu ermitteln" / "Refresh"
  LocationNoData,             // "Noch nicht ermittelt." / "Not determined yet."
  LocationRefreshing,         // "Suche..." / "Looking up..."

  // Sport mode
  SportTitle,                 // "Sport" / "Sport"
  SportSelectTitle,           // "Was machen wir?" / "What do we do?"
  SportExSquat,               // "Kniebeugen" / "Squats"
  SportExJump,                // "Huepfen" / "Jumping"
  SportExYoga,                // "Stillhalten" / "Hold still"
  SportSafetyHold,            // "Goo-Goo festhalten!" / "Hold Goo-Goo tight!"
  SportSafetyHoldEnd,         // "festhalten!" / "tight!" — 2nd line, paired with TARGET_NAME
  SportPromptSquat,           // "Mach Kniebeugen!" / "Do squats!"
  SportPromptJump,            // "Spring hoch!" / "Jump up!"
  SportPromptYoga,            // "Halte still!" / "Hold still!"
  SportSquatDown,             // "Runter!" / "Down!"
  SportSquatUp,               // "Hoch!" / "Up!"
  SportJumpGo,                // "Spring!" / "Jump!"
  SportYogaStill,             // "Stillhalten!" / "Hold still!"
  SportDoneTitle,             // "Geschafft!" / "Done!"
  SportDoneStreak,            // "Streak: " / "Streak: "
  SportDoneEnergy,            // "+30 Energie" / "+30 Energy"
  SportDoneJoy,               // "+20 Glueck" / "+20 Joy"

  // Help — sport page
  HelpTitleSport,
  HelpPSPL1, HelpPSPL2, HelpPSPL3, HelpPSPL4, HelpPSPL5,

  // Media → Freunde + Friends-mode screen
  MediaFriendsLabel,          // "Freunde" / "Friends"
  FriendsTitle,               // "Freunde suchen" / "Find friends"
  FriendsConnecting,          // "Verbinde mit WLAN..." / "Connecting to WiFi..."
  FriendsSearching,           // "Suche andere Pets..." / "Searching for pets..."
  FriendsFound,               // "Freund gefunden!" / "Friend found!"
  FriendsNoFriend,            // "Niemand da..." / "Nobody around..."
  FriendsCancel,              // "Abbrechen" / "Cancel"
  FriendsItemGift,            // "Geschenk" / "Gift"
  FriendsItemHeart,           // "Herz" / "Heart"
  FriendsItemFood,            // "Essen" / "Food"
  FriendsItemGame,            // "Spiel" / "Game"
  FriendsCounterPrefix,       // "Senden:" / "Send:"
  FriendsCooldownHint,        // "Warte kurz..." / "Wait a sec..."
  FriendsPrompt,              // "Schicke Geschenke" / "Send gifts" — action prompt in send mode
  FriendsRendezvousHint,      // "Tippt beide gleichzeitig!" / "Both tap at once!"
  FriendsRendezvousBtn,       // "Verabreden" / "Meet up"
  FriendsRendezvousWaiting,   // "Warte auf Freund..." / "Waiting for friend..."
  FriendsWaitTitle,           // "Warte auf Freund..." / "Waiting for friend..."
  FriendsWaitReceived,        // "Empfangen: %u" / "Received: %u"

  // Parents-help screen
  ParentsHelpTitle,           // "Eltern-Hilfe" / "For parents"
  ParentsHelpL1,              // "1. Settings ->"
  ParentsHelpL2,              // "   WLAN einrichten"
  ParentsHelpL3,              // "2. Mit goo-goo-setup"
  ParentsHelpL4,              // "   verbinden"
  ParentsHelpL5,              // "3. Browser:"
  ParentsHelpL6,              // "   192.168.4.1"
  ParentsHelpL7,              // "4. WLAN + Passwort"
  ParentsHelpL8,              // "5. Verbinden tippen"

  // WiFi captive-portal screen
  WifiSetupTitle,             // "WLAN einrichten" / "WiFi setup"
  WifiSetupHint1,             // "Verbinde dich mit:" / "Connect to:"
  WifiSetupHint2,             // "Dann oeffne im Browser:" / "Then open in browser:"
  WifiSetupActive,            // "Warte auf Eingabe..." / "Waiting for input..."
  WifiSetupConnecting,        // "Verbinde mit %s..." / "Connecting to %s..."
  WifiSetupSuccess,           // "Verbunden!" / "Connected!"
  WifiSetupFailed,            // "Fehler. Bitte erneut versuchen." / "Failed. Please retry."
  WifiSetupCancelBtn,         // "Abbrechen" / "Cancel"
  WifiSetupDoneBtn,           // "Fertig" / "Done"

  // Animal names + picker
  AnimalBear,
  AnimalCat,
  AnimalDog,
  AnimalTitle,                // "Tier waehlen" / "Choose pet"
  AnimalWelcome,              // "Willkommen!" / "Welcome!"

  // Bedtime sequence
  BedtimeTitle,               // "Genug gespielt!" / "Time's up!"
  BedtimeSubtitle,            // "Pet legt sich schlafen" / "Pet is going to sleep"
  BedtimeGoodnight,           // "Gute Nacht!" / "Good Night!"

  // Lockout screen
  LockoutTitle,
  LockoutSubtitle,
  LockoutRemainingMin,        // format: "Noch %u min" / "%u min left"
  LockoutRemainingSec,        // format: "Noch %u s"   / "%u s left"
  LockoutHint,

  // Toy picker
  ToyTitle,
  ToyNameBall,
  ToyNameMouse,
  ToyNameRattle,
  ToyNameButterfly,
  ToyNamePlush,
  ToyBoredHint,               // "Waehl was anderes!" / "Pick something else!"

  // Media picker
  MediaTitle,
  MediaMovies,
  MediaGames,
  MediaInternet,
  MediaSocial,
  MediaRadio,                   // "Radio" / "Radio" — web radio entry
  RadioConnecting,              // "Verbindet..." / "Connecting..."
  RadioPlaying,                 // "Radio" / "Radio" — status during playback
  RadioErrorWifi,               // "Kein WLAN" / "No WiFi"
  RadioErrorStream,             // "Stream offline" / "Stream offline"

  // Help — web radio page
  HelpTitleRadio,
  HelpPRadioL1, HelpPRadioL2, HelpPRadioL3, HelpPRadioL4, HelpPRadioL5,

  // Media — Photo + Gallery (cores3, visu)
  MediaCamera,                  // "Foto" / "Photo"
  MediaGallery,                 // "Galerie" / "Gallery"
  CameraShutterHint,            // "Tippen!" / "Tap!"
  CameraSaving,                 // "Speichere..." / "Saving..."
  CameraSaved,                  // "Gespeichert!" / "Saved!"
  GalleryEmpty,                 // "Noch keine Fotos" / "No photos yet"
  GalleryDelete,                // "Loeschen?" / "Delete?"
  GalleryYes,                   // "Ja" / "Yes"
  GalleryNo,                    // "Nein" / "No"

  // Help — Photo + Gallery page
  HelpTitlePhoto,
  HelpPPhotoL1, HelpPPhotoL2, HelpPPhotoL3, HelpPPhotoL4, HelpPPhotoL5,

  // Travel picker — title + scene names
  TravelTitle,
  SceneMeadow,
  SceneBedroom,
  SceneForest,
  SceneBeach,
  SceneDesert,
  SceneSpace,
  SceneCity,

  // Travel-transition headlines
  TransitionMeadow,
  TransitionBedroom,
  TransitionForest,
  TransitionBeach,
  TransitionDesert,
  TransitionSpace,
  TransitionCity,

  // Cleaning screen
  CleaningTitle,              // also reused for Help-page button label
  CleaningRemaining,          // "Noch %u zu reinigen" / "%u left to clean"
  CleaningHint,
  CleaningDoneBig,
  CleaningDoneSub,

  // Foraging hint
  ForagingHint,

  // Mini-game game-over overlay
  ActivityGameOver,           // "Vorbei!" / "Done!"
  ActivityScoreFormat,        // "Score: %u"
  ActivityBestFormat,         // "Bestes: %u" / "Best: %u"
  ActivityNewRecord,
  ActivityAgain,

  // Cross / Frogger banner
  CrossGoal,

  // Help — chrome + nav + page titles + button labels
  HelpHeader,                 // "Anleitung" / "Help" — same as SettingsHelp
  HelpNavBack,
  HelpNavNext,
  HelpTitleHello,
  HelpTitleTap,
  HelpTitleMouthEars,
  HelpTitlePet,               // Petting
  HelpTitleGestures,          // Finger gestures (circle, spread)
  HelpTitleWarming,           // Hand-warming
  HelpTitleHopChain,          // Rapid tapping → pet hops N times
  HelpTitleSinging,           // Upright + L/R tilt → pet sings
  HelpTitleMotion,
  HelpTitleRock,
  HelpTitleStanding,
  HelpTitleButtons,
  HelpTitleNeeds,
  HelpTitleGather,
  HelpTitleMinigames,
  HelpTitleBreak,
  HelpTitleVoice,             // Talk to Muffin
  HelpTitleCamera,            // Camera / face
  HelpLabelTravel,            // "Travel" verb form (page 8 button row)
  HelpLabelMinigame,
  HelpLabelFood,

  // Help — page 1: Hello!
  HelpP1L1, HelpP1L2, HelpP1L3, HelpP1L4, HelpP1L5,
  // Page 2: Tapping the pet
  HelpP2L1, HelpP2L2, HelpP2L3, HelpP2L4, HelpP2L5, HelpP2L6,
  // Page 3: Mouth and ears
  HelpP3L1, HelpP3L2, HelpP3L3, HelpP3L4, HelpP3L5,
  // Page 4: Petting
  HelpP4L1, HelpP4L2, HelpP4L3, HelpP4L4,
  // Page 5: Finger gestures (circle = somersault, 2-finger drag = wobble)
  HelpPGL1, HelpPGL2, HelpPGL3, HelpPGL4, HelpPGL5,
  // Page 6: Hand-warming (hold 2 fingers still)
  HelpPWL1, HelpPWL2, HelpPWL3, HelpPWL4, HelpPWL5,
  // Page 7: Make it hop (tap multiple times)
  HelpPHL1, HelpPHL2, HelpPHL3, HelpPHL4, HelpPHL5,
  // Page 8: Make it sing (upright + tilt left/right → singing + applause)
  HelpPSL1, HelpPSL2, HelpPSL3, HelpPSL4, HelpPSL5,
  // Page 9: Motion
  HelpP5L1, HelpP5L2, HelpP5L3, HelpP5L4,
  // Page 6: Rocking
  HelpP6L1, HelpP6L2, HelpP6L3, HelpP6L4,
  // Page 7: Standing it up
  HelpP7L1, HelpP7L2, HelpP7L3, HelpP7L4, HelpP7L5, HelpP7L6,
  // Page 9: Needs
  HelpP9Joy, HelpP9Energy, HelpP9Hunger, HelpP9Hint1, HelpP9Hint2,
  // Page 10: Gather food
  HelpP10L1, HelpP10L2, HelpP10L3, HelpP10L4, HelpP10L5,
  // Page 11: Mini-games
  HelpP11L1, HelpP11L2, HelpP11L3, HelpP11L4, HelpP11L5, HelpP11L6,
  // Page 12: Break time
  HelpP12L1, HelpP12L2, HelpP12L3, HelpP12L4, HelpP12L5,
  // Page 13: Talk to Muffin (wakeword + voice control)
  HelpPVoiceL1, HelpPVoiceL2, HelpPVoiceL3, HelpPVoiceL4, HelpPVoiceL5, HelpPVoiceL6,
  // Page 14: Camera (face detection)
  HelpPCamL1, HelpPCamL2, HelpPCamL3, HelpPCamL4, HelpPCamL5,

  // Credits
  CreditsSubtitle,
  CreditsRoleIdeas,
  CreditsRoleTech,
  CreditsRoleSounds,
  CreditsRoleProgramming,

  // Time-edit screen
  TimeEditTitle,

  // Timer screens
  TimerTitle,
  TimerRemaining,             // "verbleibend" / "remaining"
  TimerStop,
  TimerCustomTitle,
  TimerCustomMin,
  TimerCustomSec,
  TimerCustomTapMin,          // "Tap = +1"
  TimerCustomTapSec,          // "Tap = +10"
  TimerReset,
  TimerStart,
  TimerAlarmTitle,            // "Wecker!" / "Alarm!"
  TimerAlarmHint,             // "Tap zum Schliessen" / "Tap to dismiss"

  // Pip companion mode (Settings page 4)
  SettingsPipMode,            // "Pip-Modus" / "Pip mode"
  PipModeTitle,               // sub-page title (= "Pip-Modus" / "Pip mode")
  PipModeBody1,               // first line of the description (size-2 fits)
  PipModeBody2,               // second line of the description (size-2 fits)
  PipModeBatteryHint,         // "Kostet ~20% Akku." / "Costs ~20% battery."
  PipModeOff,                 // "Aus" / "Off"
  PipModeOn,                  // "An" / "On"

  // Pip accessory UI (rendered on the StickC Plus 2 itself).
  PipTreatApple,              // "Apfel" / "Apple"
  PipTreatCarrot,             // "Karotte" / "Carrot"
  PipTreatBone,               // "Knochen" / "Bone"
  PipThrown,                  // brief overlay during throw animation
  PipSplashTagline,           // "fuers Pet" / "for your pet"
  PipMenuEmpty,               // "Waehle aus" / "Pick one"
  PipMenuEmptyHint,           // small footer line on Empty page
  PipMenuWand,                // "Zauberstab" / "Wand"
  PipMenuWandHint,            // small hint: "Pip auf+ab schuetteln" / "Move Pip up & down"

  CountMax,
};

constexpr int kStrCount = (int)Str::CountMax;

struct StringEntry {
  const char* de;
  const char* en;
};

// Defined in i18n.cpp.
extern const StringEntry kStrings[];
extern uint8_t g_lang;        // 0 = DE, 1 = EN

inline const char* tr(Str s) {
  return g_lang ? kStrings[(int)s].en : kStrings[(int)s].de;
}
