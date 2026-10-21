"""Friendly label layer for catalog.json (2026-10-18): plain names, one-line tooltips, units, 'Basic' vs
'Advanced' groups and per-value option labels for every class parameter, plus fixes to the field layout where the
ShadowSET snapshot left a field out (the inspector shows field i = parameter word i, so a missing field shifts all the
names after it). Run after enrich_catalog.py:  python tools/stage_editor/catalog_labels.py  (idempotent).

Keys written (read by ui2.js renderInspector): label, help, unit, group ('Basic' | 'Advanced'), optionLabels
{value: {label, tip, raw}}; plus src = where the meaning comes from: game (seen in quiet game runs), code
(read in the disassembly), data (value census of all 59 stages, agents/claude/out/field_values.json), shadowset (the
community ShadowSET names / notes, often from the game's own parameter names), guess. Coverage report:
docs/catalog/PARAMETER-NAMES.md."""
import json, re, shutil, time
from pathlib import Path

APP = Path(__file__).resolve().parent
U, S, DEG, X, F = 'units', 'seconds', 'degrees', 'x', 'fraction 0-1'


def L(label, help='', unit=None, group='Basic', src='shadowset', ol=None):
    d = dict(label=label, help=help, group=group, src=src)
    if unit: d['unit'] = unit
    if ol: d['ol'] = ol
    return d


def RAW(help='Purpose unknown; keep the original value.', src='data'):
    return L('raw', help, group='Advanced', src=src)


# ---- shared option label sets --------------------------------------------------------------------------------------
NOYES = {0: ('No', ''), 1: ('Yes', '')}
SHAPES = {0: ('Sphere', 'Size X = radius.'), 1: ('Box', 'Size X / Y / Z.'), 2: ('Cylinder', 'Size X = radius, Size Y = height.'), 3: ('Cone', '')}
BOXTYPE = {0: ('GUN', 'GUN-style box.'), 1: ('Black Arms', 'Black Arms-style box.'), 2: ('Eggman', 'Eggman-style box.')}
ITEM = {-1: ('(none)', ''), 0: ('5 rings', ''), 1: ('10 rings', ''), 2: ('20 rings', ''), 3: ('Shield', 'Normal barrier.'),
        4: ('Fire shield', ''), 5: ('Electric shield', ''), 6: ('Invincibility', ''), 7: ('Nothing', ''), 8: ('1-Up', 'Extra life.'),
        9: ('Vehicle repair', 'Heals the vehicle Shadow rides.'), 10: ('Cycling item', 'Shows each item in turn; you get the one shown.')}
WEAPON_TIP = {6: 'Unused slot.', 28: 'Unused slot - may do nothing.', 29: 'Unused slot - may do nothing.', -1: 'No weapon in this field.'}


def weapon_label(raw):
    s = raw.replace('GUNBazooka', 'GUN Bazooka').replace('Not Valid In Object', '(none)').replace('None06', 'None (unused 6)')
    s = re.sub(r' Lv(\d)', r' Lv \1', s)
    m = re.match(r'(.+?)_(.+)$', s)
    return f'{m.group(1)} ({m.group(2)})' if m else s


AUDIO = {-1: ('Current partner', 'The partner of the mission you are playing speaks.'), 0: ('Dark route', 'Only on the Dark mission.'),
         1: ('Normal route', 'Only on the Normal mission.'), 2: ('Hero route', 'Only on the Hero mission.')}
LINKBEH = {0: ('Disappear', 'Removed once its Link flag is set.'), 1: ('Appear', 'Only active once its Link flag is set.')}

# ---- enemies (EnemyBase fields are already labelled; here the class fields) ----------------------------------------
SHIELD = L('Riot shield', 'Carries a shield that blocks shots from the front.', ol=NOYES)
ENEMY = {
    (0x00, 0x64): dict(HaveShield=SHIELD, WeaponType=L('Weapon', 'What the soldier carries (and drops).', ol={
        0: ('Unarmed', ''), 1: ('Knife', 'Melee.'), 2: ('Pistol', ''), 3: ('Machine gun', ''), 4: ('Rifle', ''), 5: ('Grenade launcher', ''), 6: ('Missile launcher', 'Bazooka.')})),
    (0x00, 0x8D): dict(HaveShield=SHIELD, WeaponType=L('Weapon', 'What the soldier carries (and drops).', ol={
        0: ('Unarmed', ''), 1: ('Black Sword', 'Melee.'), 2: ('Light Shot', ''), 3: ('Flash Shot', ''), 4: ('Black Barrel', ''), 5: ('Splitter', ''),
        6: ('Vacuum Pod', ''), 7: ('Heavy Shot', ''), 8: ('Ring Shot', '')}),
        IsOnAirSaucer=L('Rides a saucer', 'Arrives on a Black Arms air saucer.', ol={0: ('No', ''), 1: ('Yes', ''), 2: ('Keep original', 'A value found only on original records; leave it.')})),
    (0x00, 0x68): dict(WeaponType=L('Weapon', 'Arm weapon of the robot.', ol={0: ('Auto rifle', ''), 1: ('Anti-air rifle', ''), 2: ('Bazooka', ''), 3: ('4-shot rockets', ''), 4: ('8-shot rockets', ''), 5: ('Laser rifle', '')}),
                       BodyType=L('Body', 'Size / model of the robot.', ol={0: ('GUN Trooper', 'Normal robot.'), 1: ('Giga Trooper', 'Large robot.')})),
    (0x00, 0x79): dict(HaveShield=SHIELD, WeaponType=L('Weapon', 'What the Egg Pawn carries.', ol={0: ('Unarmed', ''), 1: ('Egg Gun', ''), 2: ('Egg Bazooka', ''), 3: ('Egg Lance', 'Melee.')})),
    (0x00, 0x65): dict(ActionType=L('Attack', 'How the Beetle attacks.', ol={0: ('None', 'Does not attack.'), 1: ('Electric shock', 'Discharges sparks (see Spark fields).'), 2: ('Use weapon', 'Fires its Weapon.')}),
                       IsGolden=L('Golden Beetle', 'The gold variant (a bonus target).', ol=NOYES),
                       WeaponType=L('Weapon', 'Used when Attack = Use weapon.', ol={0: ('Rifle', ''), 1: ('4-shot rockets', ''), 2: ('Bomb', ''), 3: ('Machine gun', ''), 4: ('None', '')}),
                       SparkDischarge=L('Spark time', 'How long an electric shock lasts.', S, src='guess'),
                       SparkWait=L('Spark pause', 'Pause between electric shocks.', S, src='guess')),
    (0x00, 0x8E): dict(ActionType=L('Attack', '', ol={0: ('None', 'Does not attack.'), 1: ('Air cutter', 'Throws air blades.')}),
                       BodyAndDeathType=L('Body / when beaten', 'Model and what happens when it is beaten.', ol={
                           0: ('Black Hawk - vanishes', ''), 1: ('Black Hawk - falls', 'Crashes down.'), 16: ('Black Volt - vanishes', ''), 17: ('Black Volt - falls', 'Crashes down.')})),
    (0x00, 0x8F): dict(ActionType=L('Attack', '', ol={0: ('None', 'Does not attack.'), 1: ('Attack', 'Shoots at you.')})),
    (0x00, 0x66): dict(WeaponType=L('Weapon', '', ol={0: ('Vulcan', 'Gatling gun.'), 1: ('Missiles', '')})),
    (0x00, 0x8C): dict(WeaponType=L('Weapon', '', ol={0: ('Black Sword', ''), 1: ('Dark Hammer', ''), 2: ('Big Barrel', '')}),
                       CanBlockShots=L('Blocks shots', 'Guards against gunfire.', ol=NOYES)),
    (0x00, 0x90): dict(WormType=L('Worm', 'Colour / strength of the worm.', ol={0: ('Black', ''), 1: ('Blue', ''), 2: ('Gold', '')}),
                       AttackCount=L('Attacks', 'How many times it strikes before retreating.', src='guess'),
                       AppearDelay=L('Appear delay', 'Wait before it bursts out.', S, src='guess')),
    (0x00, 0x91): dict(NumberOfLarva=L('Larvae', 'How many larvae it releases.'), AppearRange=L('Appear range', 'Spread of the larvae around the spot.', U),
                       Offset_Y=L('Drop height', 'Height they start / drop from (Behaviour = Drop).', U, src='guess')),
    (0x00, 0x92): dict(Health=L('Health', 'Hit points of the Chaos.', src='shadowset'),
                       StartingState=L('Starts', '', ol={0: ('Broken up', 'Starts as pieces that join together.'), 1: ('Complete', 'Starts whole.')}),
                       NumberOfChibi=L('Small pieces', 'How many small Chaos it splits into.'), BrokenPieceFlyDistance=L('Split distance', 'How far the pieces fly.', U),
                       BrokenPieceFlyOffset=L('Split height', 'Height offset of the flying pieces.', U), CombineStartTime=L('Rejoin after', 'Time before the pieces rejoin.', S),
                       CombineTime=L('Rejoin time', 'How long rejoining takes.', S)),
    (0x00, 0x93): dict(ShootCount=L('Shots', 'Shots per attack.'), AttackInterval=L('Attack interval', '', S), WaitInterval=L('Wait interval', '', S)),
    (0x00, 0x78): dict(UnknownInt=RAW('0 / 1 in the originals, no visible effect (ShadowSET).'), BombTime=L('Bomb time', 'Fuse of its bombs (official name BakudanTime).', S)),
}
NINJA_UNUSED = {n: RAW('Not read by the game (ShadowSET: unused).', src='shadowset') for n in
                ('UNUSED_Pos0_IntWaitType', 'UNUSED_Pos0_DisappearTime', 'UNUSED_Pos1_X', 'UNUSED_Pos1_Y', 'UNUSED_Pos1_Z', 'UNUSED_Pos1_WaitType', 'UNUSED_Pos1_DisappearTime', 'UNUSED_Float21', 'UNUSED_Float22')}
ENEMY[(0x00, 0x93)].update(NINJA_UNUSED)

# ---- gameplay objects ------------------------------------------------------------------------------------------------
SPRING = dict(Strength=L('Power', 'Launch strength (originals 0.5-14).', src='data'), NoControlTime=L('No control', 'Time the stick does nothing after the launch.', S, src='data'))
ART = 'Index into this stage\'s art table (ARTISTGADGET / ARTISTWORK.BIN): which piece is drawn.'
OBJ = {
    (0x25, 0x89): dict(DestructableType=L('Art piece', ART, src='code')),
    (0x25, 0x88): dict(DecorationType=L('Art piece', ART, src='code'), ScaleX=L('Scale X', '', X), ScaleY=L('Scale Y', '', X), ScaleZ=L('Scale Z', '', X)),
    (0x25, 0x8A): dict(EffectType=L('Effect', 'Index into this stage\'s effect list.', src='shadowset'), ScaleX=L('Scale X', '', X), ScaleY=L('Scale Y', '', X), ScaleZ=L('Scale Z', '', X)),
    (0x00, 0x50): dict(TriggerType=L('Effect', 'What happens inside the volume.', ol={
        0: ('Solid wall', 'Invisible wall.'), 2: ('Set Link flag', 'Entering sets the flag in Link flag (doors, cases, generators react).'),
        3: ('Hurt', 'Damages Shadow.'), 4: ('Kill', 'Instant death (pits).'), 5: ('Chaos Control cancel on', 'Turns on the Chaos Control cancel area.'),
        6: ('Chaos Control cancel off', 'Turns it off again.'), 7: ('Chaos Control stop', 'Ends a Chaos Control warp here.'),
        9: ('Keep skydiving', 'Shadow stays in the skydive state.'), 10: ('Lock controls', 'No control while inside.'), 11: ('Complete mission', 'Clears the mission on entry.')}),
        Shape=L('Shape', '', ol=SHAPES), Size_X=L('Size X', 'Sphere / cylinder: radius.', U), Size_Y=L('Size Y', 'Box: height; cylinder: height; unused for a sphere.', U),
        Size_Z=L('Size Z', 'Box only.', U), TriggerLinkBehavior=L('On Link flag', 'What it does when its Link flag is set (4 originals use Appear).', ol=LINKBEH, src='data'), Affect_LinkID=L('Link flag', 'Effect Set Link flag: the flag it sets. Other effects: the flag that switches it on / off.', src='code')),
    (0x00, 0x51): dict(Shape=L('Shape', '', ol=SHAPES), Size_X=L('Size X', 'Sphere / cylinder: radius.', U), Size_Y=L('Size Y', '', U), Size_Z=L('Size Z', '', U),
                       AudioBranchID=L('Voice line', 'Which line is spoken (id in the voice table).'), AudioBranchType=L('Speaker / route', '', ol=AUDIO),
                       float_06=RAW('10 on almost every original.'), int_07=RAW('0 on almost every original.'), int_08=RAW('7 on almost every original.'),
                       TriggerLinkBehavior=L('On Link flag', 'With a Link ID: what the trigger does when that flag is set.', ol=LINKBEH, src='shadowset')),
    (0x00, 0x11): dict(AudioBranchID=L('Hint line', 'Which hint is spoken (id in the voice table).'), AudioBranchType=L('Speaker / route', '', ol=AUDIO),
                       Float_02=RAW('Always 10 in the originals.'), Float_03=RAW('0 in the originals (one 1).'), Int_04=RAW('Always 7 in the originals.')),
    (0x00, 0x10): dict(RingType=L('Layout', 'How the rings are laid out.', ol={0: ('Single', 'One ring.'), 1: ('Line', 'A straight line along local -Z.'), 2: ('Circle', 'A ring of rings.'), 3: ('Arch', 'A curved arc.')}),
                       NumberOfRings=L('Rings', 'How many rings.'), LengthRadius=L('Length / radius', 'Line length, or circle radius.', U),
                       Angle=L('Arch bend', 'How far an Arch curves (0 for lines / circles).', src='shadowset'),
                       Ghost=L('Ghost rings', 'Draws the translucent ghost-ring model (50 of 990 originals).', ol=NOYES, src='data')),
    (0x01, 0x2C): dict(ModelIterUnused=RAW('Never used (always 0).', src='shadowset'), DropType=L('Drops weapon', 'Weapon left when it is broken (signs, poles, rods...).')),
    (0x00, 0x62): dict(SwitchMode=L('Switching', 'When the light / effect changes.', ol={0: ('While inside', 'Only while Shadow is in the volume.'), 1: ('Turn on', 'Switches on when entered and stays on.'), 2: ('Turn off', 'Switches off when entered.')}),
                       RangeShape=L('Shape', '', ol={0: ('Box', ''), 1: ('Sphere', ''), 2: ('Cylinder', '')}), LightingIsEnabled=L('Change light', 'Apply the light preset.', ol=NOYES),
                       LightNumber=L('Light preset', 'Preset from the stage\'s light.bin.', ol={0: ('Player 0', ''), 1: ('Player 1', ''), 2: ('Player 2', ''), 3: ('Player 3', '')}),
                       EffectIsEmitting=L('Emit effect', '', ol=NOYES), EffectNumber=L('Effect', 'Effect id to emit.'),
                       RangeX=L('Size X', '', U), RangeY=L('Size Y', '', U), RangeZ=L('Size Z', '', U)),
    (0x00, 0x0C): dict(BoxType=L('Style', '', ol=BOXTYPE), Weapon=L('Weapon', 'Weapon inside.')),
    (0x00, 0x09): dict(BoxType=L('Style', '', ol=BOXTYPE), BoxItem=L('Contents', 'What breaking it gives.', ol={-1: ('(none)', ''), 0: ('Nothing', ''), 1: ('Item capsule', 'Item = Contents value.'),
                       2: ('Weapon', 'Weapon number = Contents value.'), 3: ('Rings', 'Number of rings = Contents value.'), 4: ('Heal unit', ''), 5: ('Energy core', 'Hero / Dark core = Contents value.'), 6: ('Special weapon', 'Shadow\'s unlocked special weapon.')}),
                       ItemTypeModifier=L('Contents value', 'Item / weapon / ring count / core, as Contents says.')),
    (0x00, 0x0A): dict(BoxType=L('Style', '', ol=BOXTYPE), BoxItem=L('Contents', 'Same choices as the Wood Box (field found in the data, not in ShadowSET).', src='data'),
                       ItemTypeModifier=L('Contents value', 'As for the Wood Box.', src='data')),
    (0x00, 0x0B): dict(BoxType=L('Style', '', ol=BOXTYPE)),
    (0x00, 0x0F): dict(PlatformType=L('Model', 'Platform model of this stage (0-3).'), MovementType=L('Movement', '', ol={
        0: ('Back and forth', 'Moves to the offset and back.'), 1: ('Along a path', 'Follows the spline in Spline ID (needs a platform path nearby).'), 2: ('Path loop', 'Follows the spline round and round.'),
        3: ('Once on Link flag', 'Moves to the offset once its Link flag is set.'), 4: ('Back and forth (synced)', 'Like back and forth, in step with the stage clock.'),
        5: ('Path (synced)', ''), 6: ('Back and forth (pauses)', 'Pauses at each end.')}),
        TravelTime_Int=L('Travel time', 'Seconds to reach the offset.', S, src='data'),
        WaitTime=L('Wait', 'Pause at each end.', S), Float10=L('Start point', 'Where on the route it starts (official InitPos, 0-1).', F),
        Float14=L('Pause when hit', 'Official pauseDamage; 0 in almost all originals.', group='Advanced'),
        TranslationX=L('Offset X / Spline ID', 'Back-and-forth: offset X. Path movement: the spline id.', U), TranslationY=L('Offset Y', '', U), TranslationZ=L('Offset Z', '', U),
        Float24=RAW('0 on almost every original (2, 3 on a few).')),
    (0x13, 0x92): dict(Model=L('Model', 'Debris piece.'), RotDegSpdX=L('Spin X', '', 'deg/s'), RotDegSpdY=L('Spin Y', '', 'deg/s'), RotDegSpdZ=L('Spin Z', '', 'deg/s')),
    (0x00, 0x1B): dict(RoadblockType=L('Style', '', ol={0: ('GUN', ''), 1: ('Black Arms', '')})),
    (0x11, 0x33): dict(LockType=L('Opens', '', ol={0: ('When Shadow is near', 'Normal automatic door.'), 1: ('After its Link flag', 'Locked until the Link flag is set (e.g. enemies with that Link ID beaten).')}, src='code'),
                       Model=L('Model', ''), DetectRange_X=L('Detect X', 'Size of the area that opens it.', U), DetectRange_Y=L('Detect Y', 'Upwards only.', U), DetectRange_Z=L('Detect Z', '', U),
                       Offset_X=L('Area offset X', '', U), Offset_Y=L('Area offset Y', '', U), Offset_Z=L('Area offset Z', '', U)),
    (0x00, 0x03): dict(**SPRING), (0x00, 0x01): dict(**SPRING), (0x00, 0x02): dict(**SPRING),
    (0x00, 0x04): dict(Strength=SPRING['Strength'], Height=L('Lift', 'Extra upward launch (originals 0-60).', src='data'), NoControlTime=SPRING['NoControlTime']),
    (0x00, 0x05): dict(Number=L('Checkpoint number', 'Order along the stage (1 = first). Restarts use the highest touched.', src='data')),
    (0x07, 0xD7): dict(DirectionType=L('Orientation', '', ol={0: ('Floor', 'Upper Way: a floor tile.'), 1: ('Wall', 'Side Way: turned 90 degrees.')}, src='code'),
                       AppearAngleX=L('Appear angle X', 'Direction it slides in from.', DEG, src='code'), AppearAngleY=L('Appear angle Y', '', DEG, src='code')),
    (0x07, 0xD2): dict(Color=L('Colour', '', ol={0: ('Red', ''), 1: ('Yellow', ''), 2: ('Green', ''), 3: ('Blue', '')}),
                       PanelType=L('Panel', '', ol={0: ('Switch', 'Pressing it switches same-colour barriers.'), 1: ('Mover', 'A moving panel.')}),
                       Acceleration=L('Acceleration', 'Mover only.'), MoveVectorX=L('Move X', 'Mover only.', U), MoveVectorY=L('Move Y', '', U), MoveVectorZ=L('Move Z', '', U)),
    (0x00, 0x12): dict(Item=L('Item', '', ol=ITEM)),
    (0x11, 0x30): dict(Model=L('Model', 'Fence piece.')),
    (0x00, 0x4F): dict(Vehicle=L('Vehicle', '', ol={0: ('None', ''), 1: ('Jeep', ''), 2: ('Sports car', ''), 3: ('Armored car', ''), 4: ('Motorcycle', ''), 5: ('Jump mech', ''), 6: ('Armed mech', ''),
                       7: ('Air saucer', ''), 8: ('Black Hawk', ''), 9: ('Black Volt', ''), 10: ('GUN turret', ''), 11: ('Black Arms turret', ''), 12: ('GUN lift (fast)', ''), 13: ('GUN lift (slow)', '')})),
    (0x01, 0x90): dict(Partner=L('Partner', 'Who joins Shadow here.'), DetectRange=L('Join range', 'How close Shadow must come.', U)),
    (0x23, 0x97 + 0x100): {},
    (0x25, 0x97): dict(AudioID=L('Sound', 'Looping sound id.'), RangeTypeMaybeRaw=RAW('Always 0 (sphere?) in the originals.'),
                       TypeMaybeRaw=L('Mode', '0 flat damping, 1 pan control (ShadowSET).', group='Advanced'), HalfLengthX=L('Size X', 'Half size of the sound area.', U),
                       HalfLengthY=L('Size Y', '', U), HalfLengthZ=L('Size Z', '', U), FlatDampingRate=L('Fade', 'Volume falloff (0-1).', F)),
    (0x25, 0x98): dict(AudioID=L('Sound', 'One-shot sound id.'), RangeTypeMaybeRaw=RAW('Always 0 in the originals.'), TypeMaybeRaw=L('Repeat', '0 once only, 1 again on each entry (ShadowSET).', group='Advanced'),
                       HalfLengthX=L('Size X', 'Half size of the trigger area.', U), HalfLengthY=L('Size Y', '', U), HalfLengthZ=L('Size Z', '', U)),
    (0x13, 0xEF): dict(Model=L('Model', ''), Length=L('Beam length', '', U), RotTime=L('Turn time', 'Seconds per sweep (0 = fixed).', S), Rot_Y=L('Sweep angle', '', DEG),
                       MoveSec=L('Move time', 'Seconds per move (0 = fixed).', S), Translate_X=L('Move X', '', U), Translate_Y=L('Move Y', '', U), Translate_Z=L('Move Z', '', U),
                       LaserType=L('Beam', '', ol={0: ('Blocked by objects', 'Stops at objects, with a spark.'), 1: ('Passes through', 'No spark.'), 2: ('Passes through, sparks', '')})),
    (0x03, 0xEA): dict(HurtPlayer=L('Hazard', '', ol={0: ('Yes', 'Hits Shadow.'), 1: ('No', 'Scenery only.')}, src='data'), DetectRadius=L('Target ring', 'Size of the ground target ring.', U), Delay=L('Delay', 'Before it fires.', S)),
    (0x00, 0x5A): dict(Length=L('Length', 'Extends equally up and down.', U)),
    (0x0B, 0xBC): dict(Model=L('Model', ''), DetectRadius=L('Pop-up range', '', U)),
    (0x00, 0x13): dict(BalloonType=L('Movement', '', ol={0: ('Move once', 'Moves to the offset, then disappears.'), 1: ('Back and forth', ''), 2: ('Orbit', 'Circles at Orbit distance.')}),
                       Item=L('Item', '', ol=ITEM), SpeedDampAmount=L('Speed damping', '', src='shadowset'), OrbitDistance=L('Orbit distance', '', U),
                       TranslationX=L('Move X', '', U), TranslationY=L('Move Y', '', U), TranslationZ=L('Move Z', '', U)),
    (0x00, 0x3A): {},
    (0x07, 0xDF): dict(Scale_X=L('Scale X', '', X), Scale_Y=L('Scale Y', '', X), Scale_Z=L('Scale Z', '', X), SearchRange=L('Wake range', 'Rises when Shadow is this close.', U), Initial_Y=L('Start depth', 'Starts this far below and rises into place.', U)),
    (0x0F, 0xA1): dict(DetectRange=L('Trigger range', '', U)),
    (0x17, 0xD5): dict(Translate_X=L('Move X', '', U), Translate_Y=L('Move Y', '', U), Translate_Z=L('Move Z', '', U), CycleTime=L('Cycle time', 'There and back.', S)),
    (0x00, 0x07): dict(CaseType=L('Style', 'Opens when its Link flag is set (e.g. the enemies with its Link ID are beaten).', ol={0: ('Black Arms', ''), 1: ('GUN', '')}, src='code'),
                       ScaleX=L('Scale X', '', X), ScaleY=L('Scale Y', '', X), ScaleZ=L('Scale Z', '', X)),
    (0x07, 0xD5): dict(BlockType=L('Block', 'Block model (0-9).'), Scale_X=L('Scale X', '', X), Scale_Y=L('Scale Y', '', X), Scale_Z=L('Scale Z', '', X),
                       SearchRange=L('Wake range', '', U), Initial_Y=L('Start offset', 'Starts this far away on Y and moves into place.', U), CollisionOn=L('Solid', 'Only block 6 is known to use it.', ol=NOYES)),
    (0x0C, 0x80): dict(AppearType=L('Appears', '', ol={0: ('Always', ''), 1: ('After its Link flag', '')}), Strength=SPRING['Strength'], NoControlTime=SPRING['NoControlTime'], Angle=L('Angle', '', DEG)),
    (0x00, 0x15): dict(ActivateType=L('Switch', '', ol={0: ('On / off', 'Toggles.'), 1: ('On touch', ''), 2: ('Always on', ''), 3: ('Decoration', 'Does nothing.')})),
    (0x00, 0x19): dict(MoveType=L('Movement', '', ol={0: ('Up and down', ''), 1: ('Waits for Shadow', ''), 2: ('Never moves', '')}), Height=L('Rise', '', U), WaitTimeTop=L('Wait at top', '', S),
                       WaitTimeBottom=L('Wait at bottom', '', S), WaitTimeIfShot=L('Wait when shot', '', S), u5_int=L('Stays down', '1 = never rises again.', group='Advanced'),
                       ScaleX=L('Scale X', '', X), ScaleY=L('Scale Y', '', X), ScaleZ=L('Scale Z', '', X)),
    (0x00, 0x14): dict(EmeraldColor=L('Chaos Emerald', 'Emerald shown in the goal ring (None = plain goal).', ol={0: ('Blue', ''), 1: ('Green', ''), 2: ('Purple', ''), 3: ('Red', ''), 4: ('Aqua', ''), 5: ('Yellow', ''), 6: ('White', ''), 7: ('None', '')})),
    (0x00, 0x20): dict(Weapon=L('Weapon', 'Weapon lying here.')),
    (0x07, 0xE1): {},
    (0x00, 0x16): dict(Move_X=L('Move X', '', U), Move_Y=L('Move Y', '', U), Move_Z=L('Move Z', '', U), MoveSpeed=L('Move speed', ''), MoveWait=L('Wait', '', S), NumberOfHits=L('Hits', 'Shots needed.')),
    (0x00, 0x0E): dict(Angle=L('Angle', '', DEG), Distance=L('Distance', 'Flight distance.', U)),
    (0x00, 0x23): dict(Model=L('Model', ''), UnusedInt=RAW('Always 0.')),
    (0x00, 0x61): dict(Radius=L('Radius', 'Only where a dark-spin spline exists.', U)),
    (0x11, 0x34): dict(Kind=L('Kind', 'Always 0 in the originals.', group='Advanced')),
    (0x0B, 0xBE): dict(Chao=L('Chao', '', ol={0: ('Chao', ''), 1: ('Cheese', 'Cream\'s Chao.')}), MoveRadius=L('Wander radius', '', U), MoveSpeed=L('Speed', 'Negative = the other way round.')),
    (0x11, 0x38): dict(Model=L('Model', ''), Strength=SPRING['Strength'], NoControlTimeAfterLaunch=SPRING['NoControlTime'], AngleX=L('Angle X', '', DEG), Unused_AngleY=RAW('Not read (always 0).', 'shadowset'), AngleZ=L('Angle Z', '', DEG)),
    (0x11, 0x37): dict(Strength=SPRING['Strength'], Angle=L('Angle', '', DEG), NoControlTimeAfterLaunch=SPRING['NoControlTime'], Length=L('Pull-back', 'How far the sling stretches.'), Charge=L('Charge', 'Wait when fully pulled before the launch.', S)),
    (0x0C, 0x88): dict(SplineID=L('Spline ID', 'Path the zipline follows.'), MoveSpeed=L('Speed', '', 'units/s'), LaunchSpeed=L('Launch speed', '', 'units/s'), LaunchAngle=L('Launch angle', '', DEG), LaunchNoControlTime=L('No control', '', S)),
    (0x25, 0x93): dict(EnemyToSpawnLinkID=L('Enemies to spawn', 'Link ID of the enemy records it copies (give those enemies this Link ID).', src='code'),
                       LinkIDToStart=L('Start on Link flag', 'Starts spawning when this flag is set (often a Set-Link-flag trigger).', src='code'),
                       NumberOfTimesToRespawn=L('Respawns', '0 = unlimited.'), WaitTimeUntilNextRespawn=L('Respawn delay', '', S), ScoreId=RAW('Official name scoreId; always 0.', 'shadowset')),
    (0x00, 0x08): dict(StartingLength=L('Rope start', ''), EndingLength=L('Rope end', ''), LetGoAngle=L('Let-go angle', '', DEG), LetGoLaunchForce=L('Launch force', '')),
    (0x0F, 0xA4): dict(Model=L('Model', ''), ScaleX=L('Scale X', '', X), ScaleY=L('Scale Y', '', X), ScaleZ=L('Scale Z', '', X)),
    (0x13, 0xF0): dict(Model=L('Model', ''), SecBegin=L('Slide-out time', '', S), SecWait=L('Wait', '', S)),
    (0x08, 0x37): dict(ObjectType=L('Pillar', '', ol={0: ('Flat', ''), 1: ('Slope', ''), 2: ('Air', '')}), FallDirection=L('Falls to', '', ol={0: ('Left', ''), 1: ('Right', '')}),
                       DetectRadius=L('Trigger radius', '', U), DetectHeight=L('Trigger height', '', U), RollingSpeed=L('Roll speed', ''), RollingDistance=L('Roll distance', '', U),
                       **{f'Stone_{i}': L(f'Stone {i}', 'Soft pieces break, hard ones do not.', ol={0: ('Soft', ''), 1: ('Hard', '')}) for i in range(2, 7)}),
    (0x08, 0x99): dict(DetectX=L('Size X', '', U), DetectY=L('Size Y', '', U), DetectZ=L('Size Z', '', U),
                       ActionID=L('Command', 'Order to the Black Tank when Shadow passes.', ol={0: ('Move', 'Cancels a Stop (value 1 = 0).'), 1: ('Stop', 'Value 1 = seconds (0 = until Move).'), 2: ('Accelerate', 'Value 1 = seconds, value 2 = speed.'), 3: ('Jump to route', 'Value 1 = route, value 2 = position 0-1.')}),
                       ActionParam0=L('Value 1', 'See Command.'), ActionParam1=L('Value 2', 'See Command.')),
    (0x14, 0x51): dict(DetectX=L('Size X', '', U), DetectY=L('Size Y', '', U), DetectZ=L('Size Z', '', U),
                       ActionID=L('Command', 'Order to the Egg Balloon when Shadow passes.', ol={0: ('Move', ''), 1: ('Stop', 'Value 1 = seconds.'), 2: ('Missile attack', 'Value 1 = seconds, value 2 = interval.'), 3: ('Bomb attack', 'Value 1 = seconds, value 2 = interval.'), 4: ('Unknown 4', ''), 5: ('Unknown 5', ''), 6: ('Accelerate', ''), 7: ('Jump to route', '')}),
                       ActionParam0=L('Value 1', 'See Command.'), ActionParam1=L('Value 2', 'See Command.')),
    (0x13, 0x8B): dict(MaxTravelDistance=L('Travel distance', '', U), MoveSpeed=L('Speed', '', 'units/s'), Direction_X=L('Direction X', '', DEG), Direction_Y=L('Direction Y', '', DEG),
                       RotSpd_X=L('Spin X', '', 'deg/s'), RotSpd_Z=L('Spin Z', '', 'deg/s'), RotSpd_Y=L('Spin Y', '', 'deg/s'), Lifetime=L('Lifetime', 'Until the meteor explodes.', S)),
    (0x18, 0x9F): dict(Model=L('Size', '', ol={0: ('Small', ''), 1: ('Large', '')}), MoveLengthX=L('Move X', '', U), MoveLengthY=L('Move Y', '', U), MoveLengthZ=L('Move Z', '', U), MoveSec=L('Move time', '', S),
                       WaitSec=L('Wait', '', S), RotationType=L('Rotation', '', ol={0: ('One way', 'Keeps turning.'), 1: ('Back and forth', 'Turns to Max and back.')}),
                       RotationAxis=L('Axis', '', ol={0: ('X', ''), 1: ('Y', ''), 2: ('Z', '')}), RotationSpeed=L('Turn speed', '', 'deg/s'), RotationMax=L('Max angle', 'Back-and-forth only.', DEG)),
    (0x01, 0x38 + 0x100): {},
    (0x13, 0x8E): dict(Model=L('Model', '', ol={0: ('Ground', ''), 1: ('Air', '')}), DetectRange=L('Range', '', U)),
    (0x00, 0x34): {},
    (0x17, 0x72): dict(Detect_X=L('Detect X', '', U), Detect_Y=L('Detect Y', '', U), Detect_Z=L('Detect Z', '', U), CloseSpeed=L('Close speed', 'Smaller = slower.', 'units/s')),
    (0x07, 0xD4): dict(ElecSecurityType=L('Strength', '', ol={0: ('Strong', ''), 1: ('Medium', ''), 2: ('Weak', ''), 3: ('Tikal', 'Special variant (official name).')}), DetectionRange=L('Range', '', U)),
    (0x0C, 0x82): dict(Height=L('Height', '', U), Radius=L('Radius', '', U), MinSpeed=L('Min speed', ''), MaxSpeed=L('Max speed', ''), NumberOfObjectsPerSpawn=L('Per spawn', ''),
                       Id=L('Game on', '0 = no balloons unless Game number is set.'), BalloonRandomizerSeed=L('Mix', 'Balloon mix seed (any int).', group='Advanced'), SpawningRate=L('Spawn rate', ''), LinkID_GameNum=L('Game number', 'Shooting-game id (shared with Circus Game Target).')),
    (0x25, 0x94): None, (0x00, 0x1A): None,
    (0x08, 0x36): dict(Model=L('Model', '', ol={0: ('Big', ''), 1: ('Small', ''), 2: ('Head', '')}), NumberOfObjects=L('Objects', 'How many circle around.'), Radius=L('Radius', '', U),
                       CircleSpeed=L('Circle speed', '', 'deg/s'), ObjAngSpd_X=L('Spin X', '', 'deg/s'), ObjAngSpd_Y=L('Spin Y', '', 'deg/s'), ObjAngSpd_Z=L('Spin Z', '', 'deg/s')),
    (0x10, 0x04): dict(WallType=L('Type', '', ol={0: ('Out', ''), 1: ('In', '')}), Range=L('Range', '', U)),
    (0x07, 0xD3): dict(TravelDirection=L('Direction', '', ol={0: ('Up', 'Rises along Y.'), 1: ('Sideways', 'Moves along -Z.')}), BarWidth=L('Bar width', '', U), TravelDistance=L('Distance', '', U),
                       TravelTime=L('Travel time', 'Smaller = faster.', S), SpawnBarTime=L('New bar every', '', S), FadeTime=L('Fade', '', S), Synchronize=L('Phase', '0-1 offset in the cycle.', F), NumberOfBars=L('Bars', '')),
    (0x10, 0x05): dict(ResearcherType=L('Pose', '', ol={0: ('Face up', ''), 1: ('Face down', '')})),
    (0x14, 0xB5): dict(GravityDirection=L('Gravity', 'Down direction after the switch (normal is -Y).', ol={0: ('-Y (normal)', ''), 1: ('+Y', ''), 2: ('-X', ''), 3: ('+X', ''), 4: ('-Z', ''), 5: ('+Z', '')})),
    (0x08, 0x38): dict(Active=L('Active', '', ol={0: ('Off', ''), 1: ('On', '')}), Mirror=L('Mirror', '', ol={0: ('Off', ''), 1: ('On', '')}), DetectRadius=L('Range radius', '', U), DetectHeight=L('Range height', '', U)),
    (0x14, 0xB6): dict(Size_X=L('Size X', 'Spreads both ways.', U), Size_Y=L('Size Y', '', U), Size_Z=L('Size Z', '', U), GravityDirection=L('Gravity', '', ol={0: ('-Y (normal)', ''), 1: ('+Y', ''), 2: ('-X', ''), 3: ('+X', ''), 4: ('-Z', ''), 5: ('+Z', '')})),
    (0x07, 0xEB): dict(NumberOfCubes=L('Cubes', ''), Radius=L('Radius', '', U), CircleSpeed=L('Circle speed', '', 'deg/s')),
    (0x18, 0x9E): dict(PlatformType=L('Type', '', ol={0: ('Burst', 'Explodes after the delay.'), 1: ('Hover', 'Hovers and drifts.')}), ExplosionDelay=L('Delay', '', S), HoverLength=L('Hover height', '', U),
                       HoverSec=L('Hover time', '', S), TranslationX=L('Move X', '', U), TranslationY=L('Move Y', '', U), TranslationZ=L('Move Z', '', U), TravelTime=L('Move time', '', S)),
    (0x08, 0x9A): dict(RoadType=L('Road piece', '')),
    (0x11, 0x32): dict(Model=L('Model', ''), TravelDistance=L('Distance', '', U), Speed=L('Speed', ''), DetectRange=L('Range', '', U)),
    (0x18, 0x39): dict(Model=L('Model', 'Lava piece 1-9.'), RiseAmountMax=L('Max rise', '', U), RiseAmountPerSecond=L('Rise speed', '', 'units/s')),
    (0x07, 0xD8): dict(BarrierType=L('Model', 'Seen from one side only.')),
    (0x00, 0x59): dict(Unknown_int0=RAW('Always 0.'), DrawMode=L('Chunks', 'Force the chunks on or off while inside.', ol={0: ('Hide', ''), 1: ('Show', '')}), Radius=L('Radius', '', U),
                       Chunk_1=L('Chunk 1', '-1 / 0 = none.'), Chunk_2=L('Chunk 2', ''), Chunk_3=L('Chunk 3', ''), Chunk_4=L('Chunk 4', '')),
    (0x0B, 0xBB): dict(WatchForLinkID=L('Link flag', 'Official SAME_COMID_SWITCH_NUM: lights with the same number work together.')),
    (0x0C, 0x83): dict(LinkID_GameNum=L('Game number', 'Which shooting game it counts for.')),
    (0x10, 0x06): dict(Serve=L('Gives', '', ol={0: ('Bomb', ''), 1: ('Heal unit', '')})),
    (0x14, 0xBE): dict(Length=L('Length', '', U), ActiveTime=L('On time', '', S), RechargeTime=L('Off time', '', S)),
    (0x07, 0xD1): dict(SpotOnLinkID=L('Watch Link flag', 'Official ENEMY_ID: spotting Shadow sets / uses its Link flag.', ol=NOYES), RotateRange=L('Sweep angle', '', DEG), RotateSpeed=L('Sweep speed', ''), LightLength=L('Beam length', '', U)),
    (0x07, 0xE8): dict(RotateSpeed=L('Spin speed', '')),
    (0x19, 0x01): dict(BarrierType=L('Barrier', 'Needs a Comet Barrier Switch with the same Link ID.', ol={0: ('Hero', ''), 1: ('Dark', ''), 2: ('Shadow', '')})),
    (0x08, 0x34): dict(SpinDirection=L('Spin', '', ol={0: ('Left', ''), 1: ('Right', '')}), SpinningSpeed=L('Spin speed', '', 'units/s'), UpSpeed=L('Rise speed', '', 'units/s'),
                       Cylinder_Radius=L('Radius', '', U), Cylinder_Height=L('Height', '', U), Cylinder_Offset_Y=L('Height offset', '', U), Launch_X=L('Launch angle X', '', DEG),
                       Launch_Y=L('Launch angle Y', '', DEG), Launch_Speed=L('Launch speed', '', 'units/s'), NoControlSec=L('No control', '', S)),
    (0x0F, 0xA2): dict(MoveType=L('Movement', '', ol={0: ('Straight', ''), 1: ('Circle', '')}), MoveLengthX=L('Move X / radius', '', U), MoveLengthY=L('Move Y', '', U), MoveLengthZ=L('Move Z', '', U), MoveSec=L('Move time', '', S), Int20=RAW()),
    (0x17, 0x70): {**{f'{a}{x}': L(f'{b} {x}', '', U if a.endswith('Pos') else DEG) for a, b in (('Pos', 'Camera'), ('Ang', 'Camera angle'), ('LookPos', 'Look at'), ('LookAng', 'Look angle')) for x in 'XYZ'},
                   **{f'Detect_{x}': L(f'Area {x}', 'Area where this camera is used.', U) for x in 'XYZ'}},
    (0x00, 0x33): dict(EnergyCore=L('Core', '', ol={-1: ('(none)', ''), 0: ('Hero', ''), 1: ('Dark', '')})),
    (0x03, 0xE9): dict(ObjectType=L('Object', '', ol={0: ('Bridge (15 degrees)', ''), 1: ('Bridge (45 degrees)', ''), 2: ('Building', '')})),
    (0x0B, 0xC8): dict(State=L('Role', '', ol={0: ('Start', ''), 1: ('Miss', ''), 2: ('End', '')}), X_Length=L('Size X', '', U), Y_Length=L('Size Y', '', U), Z_Length=L('Size Z', '', U)),
    (0x11, 0x31): dict(AttackRange=L('Attack range', '', U), AttackSec=L('Attack time', '', S), ReviveSec=L('Regrow time', '', S)),
    (0x07, 0xDA): dict(Height=L('Height', '', U), Radius=L('Radius', '', U), Power=L('Power', '')),
    (0x07, 0xE2): dict(Scale=L('Scale', '', X)), (0x08, 0x35): dict(Radius=L('Radius', '', U)),
    (0x10, 0x69): dict(ShipType=L('Ship', '', ol={0: ('Battleship', ''), 1: ('Mothership', '')}), RangeRadius=L('Range radius', '', U), RangeHeight=L('Range height', '', U), MoveLength=L('Move distance', '', U), MoveSpeed=L('Speed', ''), FireTiming=L('Fire every', '', S)),
    (0x10, 0x6C): dict(Model=L('Model', '', ol={0: ('Outer', ''), 1: ('Inner', '')})),
    (0x08, 0x9C): dict(Height=L('Fall height', '', U)), (0x0C, 0x81): dict(Speed=L('Speed', '')),
    (0x19, 0x03): dict(DetectDistance=L('Vanish distance', 'Disappears when Shadow is this close.', U), VoiceID=L('Voice line', '')),
    (0x13, 0xED): dict(SplineID=L('Route', 'Rail route it switches the escape pod to.')),
    (0x0B, 0xC7): dict(MinSpeed=L('Min speed', '', 'units/s'), MaxSpeed=L('Max speed', '', 'units/s'), Accel=L('Acceleration', '', 'units/s2'), Decel=L('Deceleration', '', 'units/s2')),
    (0x10, 0x6D): dict(Range_X=L('Width', '', U), Range_Y=L('Height', '', U), Range_Z=L('Depth', '', U)),
    (0x13, 0xF3): dict(Distance=L('Distance', '', U), Speed=L('Speed', '')),
    (0x17, 0x73): dict(WallStartAdditionalSize=L('Start gap', '', U), CloseRate=L('Close rate', '')),
    (0x25, 0x95): dict(MissionType=L('Route', '', ol={0: ('Dark', ''), 1: ('Normal', ''), 2: ('Hero', '')}), Radius=L('Radius', '', U)),
    (0x25, 0x92): dict(MissionType=L('Route', 'Debug: clears that mission on touch.', ol={0: ('Dark', ''), 1: ('Normal', ''), 2: ('Hero', ''), 3: ('Boss', '')})),
    (0x17, 0xD4): dict(DetectRangeXZ=L('Wake radius', 'Takes off when Shadow is this close (official: radius).', U), DetectRangeY=L('Wake height', '', U),
                       TimeUntilTakeOff=L('Take-off delay', '', S), Attack=L('Attack', '', ol={0: ('None', ''), 1: ('Attack', ''), 5: ('Attack 2', '')}),
                       int4=RAW('Always 0 in the originals.'), LinkID_EnemySpawning=L('Drops enemies', 'Link ID of the enemy records it drops (-1 = none; official ComID).', src='shadowset'),
                       NumberOfDrops=L('Drops', 'How many times it re-drops them.'), int7=RAW('Always 0 in the originals.'), AttackInterval=L('Attack interval', '', S)),
    (0x07, 0xE1): dict(Range=L('Touch range', '', U, src='code')),
    (0x25, 0x86): dict(Uint32=RAW('Sample object (unused).'), Sint32=RAW('Sample object (unused).')),
}
FAN = dict(DirectionType=L('Blows', '', ol={0: ('Up', ''), 1: ('Sideways', '')}), Shape=L('Shape', '', ol={0: ('Cylinder', ''), 1: ('Box', '')}), Radius=L('Radius', '', U),
           BoxTypeAirHeight=L('Box height', 'Box shape only.', U), AirHeight=L('Air height', 'Cylinder shape only.', U), AirStrength=L('Strength', ''), TimeToRun=L('On time', '0 = always.', S),
           TimeToRecharge=L('Off time', '', S), HasModel=L('Model', 'Draw the fan model.', ol=NOYES), LinkIDMakeRun=L('Run on Link flag', '-1 = always runs; else starts when that flag is set.'))
OBJ[(0x25, 0x94)] = FAN; OBJ[(0x00, 0x1A)] = FAN
OBJ = {k: v for k, v in OBJ.items() if v and k[0] < 0x100 and k[1] < 0x100}

# ---- layout fixes: fields the snapshot left out (index = parameter word) -----------------------------------------
FIELD_FIX = {
    (0x25, 0x93): [dict(name='EnemyToSpawnLinkID', type='int'), dict(name='LinkIDToStart', type='int'), dict(name='NumberOfTimesToRespawn', type='int'),
                   dict(name='WaitTimeUntilNextRespawn', type='float'), dict(name='ScoreId', type='int')],
}
FIELD_APPEND = {   # (class): fields after the existing ones, (name, type, options)
    (0x00, 0x50): [('TriggerLinkBehavior', 'enum', [[0, 'Disappear'], [1, 'Appear']])],
    (0x00, 0x51): [('int_08', 'int', None), ('TriggerLinkBehavior', 'enum', [[0, 'Disappear'], [1, 'Appear']])],
    (0x00, 0x0A): [('BoxItem', 'enum', None), ('ItemTypeModifier', 'int', None)],
}
TYPE_FIX = {(0x00, 0x0F, 'TravelTime_Int'): 'float'}   # stored as a float in every original (0 or float bits)
OPTION_FIX = {   # broken enums from the snapshot parser
    (0x00, 0x62, 'LightNumber'): [[i, n] for i, n in enumerate(['Player 0', 'Player 1', 'Player 2', 'Player 3', 'Object 0', 'Object 1', 'Object 2', 'Object 3', 'Enemy 0', 'Enemy 1', 'Enemy 2', 'Enemy 3', 'Shadows', 'Other 0', 'Other 1', 'Other 2', 'Do not use'])],
}


CLASS_MOVE = {(0x11, 0xD4): (0x17, 0xD4)}   # ShadowSET's Object17D4_BAGunShip.cs landed on 11/D4; the game's class is 17/D4 (22 records)


def apply(catalog):
    by = {(e['list'], e['type']): e for e in catalog}
    for a, b in CLASS_MOVE.items():
        if a in by and b in by and not by[b]['fields']:
            by[b]['fields'] = by[a]['fields']; by[b]['source'] = by[a]['source']; catalog.remove(by.pop(a))
    stats = dict(fields=0, enums=0)
    for k, fs in FIELD_FIX.items():
        e = by.get(k)
        if e: e['fields'] = [dict(f, order=i + 1) for i, f in enumerate(fs)]
    for k, extra in FIELD_APPEND.items():
        e = by.get(k)
        if not e: continue
        have = {f['name'] for f in e['fields']}
        for name, typ, opts in extra:
            if name in have: continue
            f = dict(name=name, type=typ, order=len(e['fields']) + 1)
            if typ == 'enum':
                f['options'] = opts or next((g['options'] for g in by[(0, 9)]['fields'] if g['name'] == name), [])
            e['fields'].append(f)
    for (l, t, name), typ in TYPE_FIX.items():
        for f in by.get((l, t), {}).get('fields', []):
            if f['name'] == name: f['type'] = typ
    for (l, t, name), opts in OPTION_FIX.items():
        for f in by.get((l, t), {}).get('fields', []):
            if f['name'] == name: f['options'] = opts
    for k, layer in {**OBJ, **ENEMY}.items():
        e = by.get(k)
        if not e: continue
        for f in e['fields']:
            d = layer.get(f['name'])
            if not d: continue
            if d.get('unit') and re.fullmatch(r'(?i)\s*(m|s|sec|deg|point|m/s(ec)?|deg/s(ec)?|m/sec\^2)\s*', f.get('help', '')): f.pop('help')
            for key in ('label', 'help', 'unit', 'group', 'src'):
                if d.get(key): f[key] = d[key]
            if d.get('ol'):
                raw = dict((v, n) for v, n in f.get('options') or [])
                f['optionLabels'] = {str(v): dict(label=lab, tip=tip, raw=raw.get(v, '')) for v, (lab, tip) in d['ol'].items()}
            stats['fields'] += 1; stats['enums'] += bool(d.get('ol'))
    # weapon enums everywhere: tidy option labels generated from the raw names
    for e in catalog:
        for f in e['fields']:
            if f['type'] == 'enum' and any(n == 'Shadow Rifle' for _, n in f.get('options') or []) and 'optionLabels' not in f:
                f['optionLabels'] = {str(v): dict(label=weapon_label(n), tip=WEAPON_TIP.get(v, ''), raw=n) for v, n in f['options']}
                stats['enums'] += 1
    # anything still unnamed with a raw-looking name: Advanced
    for e in catalog:
        for f in e['fields']:
            if not f.get('label') and re.match(r'(?i)(unk|unknown|unused|int_?\d|float_?\d|float\d|int\d|u\d|.*MaybeRaw)', f['name']):
                f.setdefault('group', 'Advanced'); f.setdefault('label', 'raw'); f.setdefault('help', 'Purpose unknown; keep the original value.'); f.setdefault('src', 'data')
    return stats


def main():
    p = APP / 'catalog.json'
    bak = APP.parents[1] / 'agents/claude/backups' / f'catalog.before-labels-{time.strftime("%Y%m%d-%H%M%S")}-{time.perf_counter_ns() % 100000}.json'
    shutil.copy2(p, bak)
    catalog = json.loads(p.read_text('utf-8'))
    stats = apply(catalog)
    p.write_text(json.dumps(catalog, ensure_ascii=False, indent=1), 'utf-8')
    print(stats, 'backup', bak.name)


if __name__ == '__main__':
    main()
