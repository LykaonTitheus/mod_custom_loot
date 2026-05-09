# mod_custom_loot for Playerbots
Are you tired of farming bosses to get the one item you are looking for? You do not have enough time to run raids every week and waste your time for Blizzards casino loot system like you did 20 years ago? This module allows you to select one item from the boss' loot table. It is customizable too!

Boss Reward System (mod_custom_loot)

A compact module for AzerothCore Playerbots that records boss kills in a database. After a boss kill you are rewarded with a reusable boss token which allows players to summon a personal reward chest. The chest features a gossip menu with alphabetically sorted loot list and item previews. The chests are temporary and are currently set to last for 60 seconds. To fill the list for loot selection, the boss reward is rolled for x times (currently set to 100 which euqals 100x the chance). Many parameters can be adjusted in the .cpp file.

1. Directory Structure

Your module should be organized exactly like this:

azerothcore-wotlk/
  └── modules/
      └── mod_custom_loot/
          ├── CMakeLists.txt
          └── src/
              └── mod_custom_loot.cpp

2. File Content

### CMakeLists.txt

This file tells the compiler where your source code is. Copy this into CMakeLists.txt and save it:

AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src/mod_custom_loot.cpp")

### src/mod_custom_loot.cpp

Ensure your full C++ code is in this file. At the very bottom, make sure the setup function matches the file name:
C++

void AddSC_mod_custom_loot()
{
    new mod_boss_reward();
    new mod_loot_item();
    new mod_loot_chest();
}

3. SQL Setup (Required)

You must execute these commands in your SQL editor for the script to function correctly.

### For the characters database:

This table stores the boss kills until the player redeems them at the chest.

CREATE TABLE IF NOT EXISTS `character_boss_tokens` (
  `player_guid` INT UNSIGNED NOT NULL,
  `boss_id` INT UNSIGNED NOT NULL,
  `kill_time` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  INDEX (`player_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

### For the world database:

Add the script name "mod_loot_chest" (without the quotation marks "") to the SQL database in the gameobject_template data, column 34 "ScriptName"(I used 181061). You can do that by executing this code on your world database:

UPDATE `gameobject_template` 
SET `ScriptName` = 'mod_loot_item' 
WHERE `entry` = 181061;

4. Follow the instructions in this video (https://www.youtube.com/watch?v=1NSf82XvJWM) to add a new custom item to your .dbc so you can have a functioning boss token (which is used to spawn a treasure chest).
In the item_template (world database) I duplicated the master health stone 22105 and gave it the ID 59000 which I added to a new patch file called patch-enUS-4.MPQ (the token now also heals you), you can change the stack size (column "stackable", I used 20) of the token and its level requirements (0) in the database, you can do it using this code on your world database:

### 1. Create the new item based on the Healthstone
REPLACE INTO `item_template` 
SELECT * FROM `item_template` WHERE `entry` = 22105;

### 2. Update the specific values for your custom token
UPDATE `item_template` 
SET 
    `entry` = 59000,
    `name` = 'Boss Token',
    `description` = 'Can be used to summon a Boss Reward Chest.',
    `stackable` = 20,
    `RequiredLevel` = 0,
    `maxcount` = 0,         -- 0 means you can carry as many stacks as you want
    `bonding` = 0,          -- 0 means it is not Soulbound (tradeable)
    `ScriptName` = ''       -- Ensure no old Healthstone scripts interfere
WHERE `entry` = 59000;

5. Installation & Compilation

    Place the mod_custom_loot.cpp in the src/ folder.

    Rerun CMake to let the build system detect the new module.

    Recompile your server (e.g., using make or Visual Studio).

    When starting the Worldserver, look for this line in the console:
    >> Loaded SCRIPTS: mod_custom_loot

5. How it Works

    Kill Recording: When a boss dies, the script saves the boss_id and player_guid into the database.

    Summoning: When the player uses the trigger (Item or Command), the script checks for a nearby chest within 15m to prevent spamming, and spawns the treasure chest containing quality-filtered loot (blue and above).

    Personalization: The chest only opens for the owner (GetOwnerGUID() == player->GetGUID()).

    Loot Simulation: The script runs a 100x loot simulation loop to ensure rare drops are included, then sorts them alphabetically (A-Z) for the player to choose. The 100x rolling can be adjusted to any desired value.

    Cleanup: When a character is deleted, their boss kill records are automatically purged from the database.
