-- Pet (same pet and pet summon) spells correct (only for use with pet scaling system!)

-- Need correct spellcasting for this!
-- UPDATE creature_template SET spell1 = 12470, spell2 = 57984 WHERE entry = 15438;
-- DELETE FROM `creature_template_spells` WHERE `entry` = 15352;
UPDATE `creature_template_spells` SET `spell1` = 40133 WHERE `entry` = 15439;
UPDATE `creature_template_spells` SET `spell1` = 40132 WHERE `entry` = 15430;

DELETE FROM `dbscripts_on_event` WHERE `id` IN (14859,14858);
INSERT INTO `dbscripts_on_event` (`id`, `delay`, `command`, `datalong`, `datalong2`, `buddy_entry`, `search_radius`, `data_flags`, `dataint`, `dataint2`, `x`, `y`, `z`, `o`, `comments`) VALUES
(14858, 1, 15, 33663, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 'Summon greater Earth elemental'),
(14859, 1, 15, 32982, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 'Summon greater Fire  elemental');

-- Valkyr guardian
UPDATE `creature_template` SET `MinLevelMana` = '6500', `MaxLevelMana` = '6500' WHERE `entry` =38391;
REPLACE INTO `creature_template_spells` SET `entry` = 38391, `spell1` = 71841;
REPLACE INTO `creature_spell` (`guid`,`spell`,`index`) VALUES (38391, 71841, 0);

-- Mirror Image
UPDATE `creature_template` SET `SpeedWalk` = 2.5, `ModelId3` = 11686, `MinLevel` = 80, `MaxLevel` = 80, `EquipmentTemplateId` = 0 WHERE `entry` = 31216;
DELETE FROM `creature_spell` WHERE `guid` IN (31216);
INSERT INTO `creature_spell` (`guid`, `spell`, `index`) VALUES
(31216, 59637, 0),
(31216, 59638, 1);

-- DK Gargoyle
DELETE FROM `creature_spell` WHERE `guid` IN (27829);
INSERT INTO `creature_spell` (`guid`, `spell`, `index`) VALUES
(27829, 51963, 0),
(27829, 43375, 1);

-- Raise Ally
-- Commit 8f022d171f3e383e2e77
UPDATE `creature_template` SET `ScriptName`='npc_risen_ally' WHERE `entry`='30230';
DELETE FROM `creature_spell` WHERE `guid` IN (30230);
INSERT INTO `creature_spell` (`guid`, `spell`, `index`, `active`, `disabled`, `flags`) VALUES
(30230, 62225, 0, 0, 0, 0),
(30230, 47480, 1, 0, 0, 0),
(30230, 47481, 2, 0, 0, 0),
(30230, 47482, 3, 0, 0, 0),
(30230, 47484, 4, 0, 0, 0),
(30230, 67886, 5, 0, 0, 0);