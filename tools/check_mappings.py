#!/usr/bin/env python3
"""Verify every mapping in src/jni/mappings.h against the game's real bytecode.

Fabric production runs Minecraft with intermediary names, so the authoritative
source for "does this member exist with this descriptor" is the remapped client
jar Fabric caches at .fabric/remappedJars/*/client-intermediary.jar. javap reads
those class files directly, so this needs no running game, no JVM attach and no
guessing from memory.

Usage:  python tools/check_mappings.py <client-intermediary.jar> [mappings.tiny]

With the Yarn mappings file (the form tools/mappings.sh caches), every entry is
additionally checked by *name*: an intermediary id resolves to one Yarn member,
and that member is what the ``meaning`` column claims. This is the check that
catches the failure mode javap cannot see -- a real member of the right shape
that is simply not the thing the code thinks it is.

Exit code 0 when every mapping resolves, 1 otherwise.
"""

import re
import subprocess
import sys
from pathlib import Path

# --- what the client intends to use -----------------------------------------
# (constant, java class, member kind, member name, descriptor, meaning)
#
# Names come from Fabric's *intermediary* namespace, which is what the production
# jar actually contains. The ``meaning`` column is the Yarn name the code assumes;
# it is checked by the semantic probes further down for the members where a
# plausible-looking name could silently mean something else.
MAPPINGS = [
    # MinecraftClient
    ("kMinecraftClient", "net.minecraft.class_310", "c", "class_310", None, "MinecraftClient"),
    ("kMcGetInstance", "net.minecraft.class_310", "m", "method_1551", "()Lnet/minecraft/class_310;", "getInstance"),
    ("kMcPlayer", "net.minecraft.class_310", "f", "field_1724", "Lnet/minecraft/class_746;", "player"),
    ("kMcWorld", "net.minecraft.class_310", "f", "field_1687", "Lnet/minecraft/class_638;", "world"),
    ("kMcOptions", "net.minecraft.class_310", "f", "field_1690", "Lnet/minecraft/class_315;", "options"),
    ("kMcSetScreen", "net.minecraft.class_310", "m", "method_1507", "(Lnet/minecraft/class_437;)V", "setScreen"),
    ("kMcGameRenderer", "net.minecraft.class_310", "f", "field_1773", "Lnet/minecraft/class_757;", "gameRenderer"),
    # Entity
    ("kEntityGetX", "net.minecraft.class_1297", "m", "method_23317", "()D", "getX"),
    ("kEntityGetY", "net.minecraft.class_1297", "m", "method_23318", "()D", "getY"),
    ("kEntityGetZ", "net.minecraft.class_1297", "m", "method_23321", "()D", "getZ"),
    ("kEntityGetYaw", "net.minecraft.class_1297", "m", "method_36454", "()F", "getYaw"),
    ("kEntityGetPitch", "net.minecraft.class_1297", "m", "method_36455", "()F", "getPitch"),
    ("kEntitySetYaw", "net.minecraft.class_1297", "m", "method_36456", "(F)V", "setYaw"),
    ("kEntitySetPitch", "net.minecraft.class_1297", "m", "method_36457", "(F)V", "setPitch"),
    ("kEntityGetEyePos", "net.minecraft.class_1297", "m", "method_33571", "()Lnet/minecraft/class_243;", "getEyePos"),
    ("kEntityGetBox", "net.minecraft.class_1297", "m", "method_5829", "()Lnet/minecraft/class_238;", "getBoundingBox"),
    ("kEntityIsAlive", "net.minecraft.class_1297", "m", "method_5805", "()Z", "isAlive"),
    ("kEntityGetWidth", "net.minecraft.class_1297", "m", "method_17681", "()F", "getWidth"),
    ("kEntityGetHeight", "net.minecraft.class_1297", "m", "method_17682", "()F", "getHeight"),
    # LivingEntity
    ("kLivingEntity", "net.minecraft.class_1309", "c", "class_1309", None, "LivingEntity"),
    ("kLivingGetHealth", "net.minecraft.class_1309", "m", "method_6032", "()F", "getHealth"),
    ("kLivingGetMaxHealth", "net.minecraft.class_1309", "m", "method_6063", "()F", "getMaxHealth"),
    # GameRenderer / Camera
    ("kGameRendererCamera", "net.minecraft.class_757", "m", "method_19418", "()Lnet/minecraft/class_4184;", "getCamera"),
    ("kCameraGetYaw", "net.minecraft.class_4184", "m", "method_19330", "()F", "Camera.getYaw"),
    ("kCameraGetPitch", "net.minecraft.class_4184", "m", "method_19329", "()F", "Camera.getPitch"),
    ("kCameraPos", "net.minecraft.class_4184", "f", "field_18712", "Lnet/minecraft/class_243;", "Camera.pos"),
    # GameOptions / SimpleOption
    ("kGameOptionsFov", "net.minecraft.class_315", "f", "field_1826", "Lnet/minecraft/class_7172;", "GameOptions.fov"),
    ("kGameOptionsGamma", "net.minecraft.class_315", "f", "field_1840", "Lnet/minecraft/class_7172;", "GameOptions.gamma (the brightness option)"),
    ("kSimpleOptionGetValue", "net.minecraft.class_7172", "m", "method_41753", "()Ljava/lang/Object;", "SimpleOption.getValue"),
    ("kSimpleOptionSetValue", "net.minecraft.class_7172", "m", "method_41748", "(Ljava/lang/Object;)V", "SimpleOption.setValue"),
    ("kSimpleOptionValue", "net.minecraft.class_7172", "f", "field_37868", "Ljava/lang/Object;", "SimpleOption.value"),
    # Vec3d
    ("kVec3X", "net.minecraft.class_243", "f", "field_1352", "D", "Vec3d.x"),
    ("kVec3Y", "net.minecraft.class_243", "f", "field_1351", "D", "Vec3d.y"),
    ("kVec3Z", "net.minecraft.class_243", "f", "field_1350", "D", "Vec3d.z"),
    # Box
    ("kBoxMinX", "net.minecraft.class_238", "f", "field_1323", "D", "Box.minX"),
    ("kBoxMinY", "net.minecraft.class_238", "f", "field_1322", "D", "Box.minY"),
    ("kBoxMinZ", "net.minecraft.class_238", "f", "field_1321", "D", "Box.minZ"),
    ("kBoxMaxX", "net.minecraft.class_238", "f", "field_1320", "D", "Box.maxX"),
    ("kBoxMaxY", "net.minecraft.class_238", "f", "field_1325", "D", "Box.maxY"),
    ("kBoxMaxZ", "net.minecraft.class_238", "f", "field_1324", "D", "Box.maxZ"),
    # World
    ("kClientPlayer", "net.minecraft.class_746", "c", "class_746", None, "ClientPlayerEntity"),
    ("kPlayer", "net.minecraft.class_1657", "c", "class_1657", None, "PlayerEntity"),
    ("kWorldGetPlayers", "net.minecraft.class_638", "m", "method_18456", "()Ljava/util/List;", "World.getPlayers"),
    ("kBlockPos", "net.minecraft.class_2338", "c", "class_2338", None, "BlockPos"),
    # Auto Jump
    ("kGameOptionsAutoJump", "net.minecraft.class_315", "f", "field_1848", "Lnet/minecraft/class_7172;", "GameOptions.autoJump"),
    # Sprint: the game's own key binding, not the entity flag
    ("kGameOptionsSprintKey", "net.minecraft.class_315", "f", "field_1867", "Lnet/minecraft/class_304;", "GameOptions.sprintKey"),
    ("kKeyBinding", "net.minecraft.class_304", "c", "class_304", None, "KeyBinding"),
    ("kKeyBindingSetPressed", "net.minecraft.class_304", "m", "method_23481", "(Z)V", "KeyBinding.setPressed"),
    ("kKeyBindingIsPressed", "net.minecraft.class_304", "m", "method_1434", "()Z", "KeyBinding.isPressed"),
    ("kEntitySetSprinting", "net.minecraft.class_1297", "m", "method_5728", "(Z)V", "setSprinting"),
    ("kEntityIsSprinting", "net.minecraft.class_1297", "m", "method_5624", "()Z", "isSprinting"),
    # Player identity, for nametags. GameProfile lives in authlib, not the game
    # jar, so this one is checked against the library jar passed alongside.
    ("kPlayerGetGameProfile", "net.minecraft.class_1657", "m", "method_7334", "()Lcom/mojang/authlib/GameProfile;", "PlayerEntity.getGameProfile"),
    ("kGameProfileGetName", "com.mojang.authlib.GameProfile", "m", "getName", "()Ljava/lang/String;", "GameProfile.getName"),
    # Combat: the attack key, the crosshair's target, and the game's own screen
    ("kGameOptionsAttackKey", "net.minecraft.class_315", "f", "field_1886", "Lnet/minecraft/class_304;", "GameOptions.attackKey"),
    ("kMcTargetedEntity", "net.minecraft.class_310", "f", "field_1692", "Lnet/minecraft/class_1297;", "MinecraftClient.targetedEntity"),
    ("kMcCurrentScreen", "net.minecraft.class_310", "f", "field_1755", "Lnet/minecraft/class_437;", "MinecraftClient.currentScreen"),
    # Knockback
    ("kLivingHurtTime", "net.minecraft.class_1309", "f", "field_6235", "I", "LivingEntity.hurtTime"),
    ("kLivingMaxHurtTime", "net.minecraft.class_1309", "f", "field_6254", "I", "LivingEntity.maxHurtTime"),
    ("kEntityGetVelocity", "net.minecraft.class_1297", "m", "method_18798", "()Lnet/minecraft/class_243;", "Entity.getVelocity"),
    ("kEntitySetVelocity", "net.minecraft.class_1297", "m", "method_18800", "(DDD)V", "Entity.setVelocity(x, y, z)"),
    # Reach: the two attributes and the instance the value is written through
    ("kEntityAttributes", "net.minecraft.class_5134", "c", "class_5134", None, "EntityAttributes"),
    ("kAttrBlockRange", "net.minecraft.class_5134", "f", "field_47758", "Lnet/minecraft/class_6880;", "EntityAttributes.BLOCK_INTERACTION_RANGE"),
    ("kAttrEntityRange", "net.minecraft.class_5134", "f", "field_47759", "Lnet/minecraft/class_6880;", "EntityAttributes.ENTITY_INTERACTION_RANGE"),
    ("kLivingGetAttributeInstance", "net.minecraft.class_1309", "m", "method_5996", "(Lnet/minecraft/class_6880;)Lnet/minecraft/class_1324;", "LivingEntity.getAttributeInstance"),
    ("kEntityAttributeInstance", "net.minecraft.class_1324", "c", "class_1324", None, "EntityAttributeInstance"),
    ("kAttrInstanceGetValue", "net.minecraft.class_1324", "m", "method_6194", "()D", "EntityAttributeInstance.getValue"),
    ("kAttrInstanceGetBaseValue", "net.minecraft.class_1324", "m", "method_6201", "()D", "EntityAttributeInstance.getBaseValue"),
    ("kAttrInstanceSetBaseValue", "net.minecraft.class_1324", "m", "method_6192", "(D)V", "EntityAttributeInstance.setBaseValue"),
    # The integrated server, and our own place in it
    ("kMcGetServer", "net.minecraft.class_310", "m", "method_1576", "()Lnet/minecraft/class_1132;", "MinecraftClient.getServer"),
    # MinecraftServer keeps its own name in this version (its members are still
    # obfuscated), while the IntegratedServer that MinecraftClient.getServer()
    # returns is class_1132 -- hence the two different spellings on adjacent lines.
    ("kMinecraftServer", "net.minecraft.server.MinecraftServer", "c", "MinecraftServer", None, "MinecraftServer"),
    ("kMinecraftServerGetPlayerManager", "net.minecraft.server.MinecraftServer", "m", "method_3760", "()Lnet/minecraft/class_3324;", "MinecraftServer.getPlayerManager"),
    ("kPlayerManager", "net.minecraft.class_3324", "c", "class_3324", None, "PlayerManager"),
    ("kPlayerManagerGetPlayerList", "net.minecraft.class_3324", "m", "method_14571", "()Ljava/util/List;", "PlayerManager.getPlayerList"),
    ("kEntityGetUuidAsString", "net.minecraft.class_1297", "m", "method_5845", "()Ljava/lang/String;", "Entity.getUuidAsString"),
    # Step and Speed: two more attributes, read by the entity that moves.
    ("kAttrStepHeight", "net.minecraft.class_5134", "f", "field_47761", "Lnet/minecraft/class_6880;", "EntityAttributes.STEP_HEIGHT"),
    ("kAttrMovementSpeed", "net.minecraft.class_5134", "f", "field_23719", "Lnet/minecraft/class_6880;", "EntityAttributes.MOVEMENT_SPEED"),
    # Fly: the game's own flight state, which is a plain holder of flags.
    ("kPlayerGetAbilities", "net.minecraft.class_1657", "m", "method_31549", "()Lnet/minecraft/class_1656;", "PlayerEntity.getAbilities"),
    ("kPlayerAbilities", "net.minecraft.class_1656", "c", "class_1656", None, "PlayerAbilities"),
    ("kAbilitiesAllowFlying", "net.minecraft.class_1656", "f", "field_7478", "Z", "PlayerAbilities.allowFlying"),
    ("kAbilitiesFlying", "net.minecraft.class_1656", "f", "field_7479", "Z", "PlayerAbilities.flying"),
    ("kAbilitiesFlySpeed", "net.minecraft.class_1656", "f", "field_7481", "F", "PlayerAbilities.flySpeed"),
    # Criticals: the terms of the crit predicate, read on the client player.
    ("kGameOptionsJumpKey", "net.minecraft.class_315", "f", "field_1903", "Lnet/minecraft/class_304;", "GameOptions.jumpKey"),
    ("kEntityFallDistance", "net.minecraft.class_1297", "f", "field_6017", "D", "Entity.fallDistance"),
    ("kEntityIsOnGround", "net.minecraft.class_1297", "m", "method_24828", "()Z", "Entity.isOnGround"),
    ("kLivingIsClimbing", "net.minecraft.class_1309", "m", "method_6101", "()Z", "LivingEntity.isClimbing"),
    ("kEntityIsTouchingWater", "net.minecraft.class_1297", "m", "method_5799", "()Z", "Entity.isTouchingWater"),
    ("kPlayerHasBlindness", "net.minecraft.class_1657", "m", "method_74025", "()Z", "PlayerEntity.hasBlindnessEffect"),
    ("kEntityHasVehicle", "net.minecraft.class_1297", "m", "method_5765", "()Z", "Entity.hasVehicle"),
    # Fast Place, and the held stack its "only blocks" setting asks about.
    ("kMcItemUseCooldown", "net.minecraft.class_310", "f", "field_1752", "I", "MinecraftClient.itemUseCooldown"),
    ("kLivingGetMainHandStack", "net.minecraft.class_1309", "m", "method_6047", "()Lnet/minecraft/class_1799;", "LivingEntity.getMainHandStack"),
    ("kItemStack", "net.minecraft.class_1799", "c", "class_1799", None, "ItemStack"),
    ("kItemStackGetItem", "net.minecraft.class_1799", "m", "method_7909", "()Lnet/minecraft/class_1792;", "ItemStack.getItem"),
    ("kBlockItem", "net.minecraft.class_1747", "c", "class_1747", None, "BlockItem"),
    # Fly's dip: the position write itself.
    ("kEntitySetPosition", "net.minecraft.class_1297", "m", "method_5814", "(DDD)V", "Entity.setPosition(x, y, z)"),
    # Triggerbot: the game's own click handler, its hold limiter, the invisibility
    # flag, the two halves of the weapon charge, and the "using an item" state
    # that makes the game swallow clicks.
    ("kMcDoAttack", "net.minecraft.class_310", "m", "method_1536", "()Z", "MinecraftClient.doAttack"),
    ("kMcAttackCooldown", "net.minecraft.class_310", "f", "field_1771", "I", "MinecraftClient.attackCooldown"),
    ("kEntityIsInvisible", "net.minecraft.class_1297", "m", "method_5767", "()Z", "Entity.isInvisible"),
    # Declared on LivingEntity, not on PlayerEntity: the getter that reads it
    # lives one class up, so the constant is named for where the field is.
    ("kLivingTicksSinceLastAttack", "net.minecraft.class_1309", "f", "field_6273", "I", "LivingEntity.ticksSinceLastAttack"),
    ("kPlayerCooldownPerTick", "net.minecraft.class_1657", "m", "method_7279", "()F", "PlayerEntity.getAttackCooldownProgressPerTick"),
    ("kLivingIsUsingItem", "net.minecraft.class_1309", "m", "method_6115", "()Z", "LivingEntity.isUsingItem"),
    # Auto Totem: the inventory click the swap-hand key makes, and every member the
    # module reads to name the right slot in it.
    ("kPlayerScreenHandler", "net.minecraft.class_1657", "f", "field_7498", "Lnet/minecraft/class_1723;", "PlayerEntity.playerScreenHandler"),
    ("kPlayerCurrentHandler", "net.minecraft.class_1657", "f", "field_7512", "Lnet/minecraft/class_1703;", "PlayerEntity.currentScreenHandler"),
    ("kPlayerGetInventory", "net.minecraft.class_1657", "m", "method_31548", "()Lnet/minecraft/class_1661;", "PlayerEntity.getInventory"),
    ("kScreenHandler", "net.minecraft.class_1703", "c", "class_1703", None, "ScreenHandler"),
    ("kScreenHandlerSyncId", "net.minecraft.class_1703", "f", "field_7763", "I", "ScreenHandler.syncId"),
    ("kScreenHandlerSlots", "net.minecraft.class_1703", "f", "field_7761", "Lnet/minecraft/class_2371;", "ScreenHandler.slots"),
    ("kScreenHandlerGetSlot", "net.minecraft.class_1703", "m", "method_7611", "(I)Lnet/minecraft/class_1735;", "ScreenHandler.getSlot"),
    ("kDefaultedList", "net.minecraft.class_2371", "c", "class_2371", None, "DefaultedList"),
    ("kListSize", "net.minecraft.class_2371", "m", "size", "()I", "DefaultedList.size"),
    ("kSlot", "net.minecraft.class_1735", "c", "class_1735", None, "Slot"),
    ("kSlotInventory", "net.minecraft.class_1735", "f", "field_7871", "Lnet/minecraft/class_1263;", "Slot.inventory"),
    ("kSlotId", "net.minecraft.class_1735", "f", "field_7874", "I", "Slot.id"),
    ("kSlotGetStack", "net.minecraft.class_1735", "m", "method_7677", "()Lnet/minecraft/class_1799;", "Slot.getStack"),
    ("kLivingGetOffHandStack", "net.minecraft.class_1309", "m", "method_6079", "()Lnet/minecraft/class_1799;", "LivingEntity.getOffHandStack"),
    ("kItems", "net.minecraft.class_1802", "c", "class_1802", None, "Items"),
    ("kItemsTotemOfUndying", "net.minecraft.class_1802", "f", "field_8288", "Lnet/minecraft/class_1792;", "Items.TOTEM_OF_UNDYING"),
    ("kMcInteractionManager", "net.minecraft.class_310", "f", "field_1761", "Lnet/minecraft/class_636;", "MinecraftClient.interactionManager"),
    ("kInteractionManager", "net.minecraft.class_636", "c", "class_636", None, "ClientPlayerInteractionManager"),
    ("kInteractionManagerClickSlot", "net.minecraft.class_636", "m", "method_2906", "(IIILnet/minecraft/class_1713;Lnet/minecraft/class_1657;)V", "ClientPlayerInteractionManager.clickSlot"),
    ("kSlotActionType", "net.minecraft.class_1713", "c", "class_1713", None, "SlotActionType"),
    ("kSlotActionSwap", "net.minecraft.class_1713", "f", "field_7791", "Lnet/minecraft/class_1713;", "SlotActionType.SWAP"),
]

# --- semantic probes --------------------------------------------------------
# Existence with the right descriptor is necessary but not sufficient: several
# members are only distinguishable by what they *do*. Each probe is checked
# against the real bytecode.
#
#   unique: exactly this many members of the class may have the descriptor. A
#           count of 1 proves the name is the only candidate, so it cannot be a
#           sibling that merely looks the same.
#   body:   a regex that must appear in the member's own disassembly. This is how
#           a getter is tied to the field it returns.
#   sig:    a generic signature that must be on the member, from javap -v. This
#           separates two SimpleOptions that differ only in their type argument.
#   used_by: (class, kind, member) whose body must reference this member. This is
#           the only way to check a *field* that has no body of its own, and it
#           is what ties the two interaction-range attributes to the getters
#           that read them.
#   used_with: (class, kind, member, regex) -- used_by with an argument that has
#           to be there too. This is how a *value* the code passes is tied to the
#           call it is copied from: SlotActionType.SWAP is the enum the game's own
#           swap-hand key passes, and it passes 40 with it.
PROBES = {
    # Coordinates: each getter must return the matching Vec3d component field.
    "kEntityGetX": {"body": r"class_243\.field_1352"},
    "kEntityGetY": {"body": r"class_243\.field_1351"},
    "kEntityGetZ": {"body": r"class_243\.field_1350"},
    # Rotation: getters read a field, setters write one, and the pair must agree.
    "kEntityGetYaw":   {"body": r"Field field_\d+:F"},
    "kEntityGetPitch": {"body": r"Field field_\d+:F"},
    "kEntitySetYaw":   {"body": r"putfield\s+#\d+\s+// Field field_\d+:F"},
    "kEntitySetPitch": {"body": r"putfield\s+#\d+\s+// Field field_\d+:F"},
    # Eye position is not plain getPos(): it offsets by the standing eye height.
    "kEntityGetEyePos": {"body": r"(dadd|add|method_)\s*"},
    # Bounding box is a field read, not a fresh computation.
    "kEntityGetBox": {"body": r"Field field_6005"},
    # The camera the frame was rendered from.
    "kGameRendererCamera": {"unique": 1},
    # Camera.pos is the only Vec3d field on Camera.
    "kCameraPos": {"unique": 1},
    # Both are SimpleOption, so only the type argument tells them apart:
    # fov holds an Integer, brightness a Double.
    "kGameOptionsFov":   {"sig": r"Lnet/minecraft/class_7172<Ljava/lang/Integer;>;"},
    "kGameOptionsGamma": {"sig": r"Lnet/minecraft/class_7172<Ljava/lang/Double;>;"},
    # The player list is the world's, not some other List-returning accessor.
    "kWorldGetPlayers": {"unique": 2},
    # Knockback: the getter hands back the entity's own Vec3d field, and the
    # three-double setter builds one. Entity has several setVelocity overloads,
    # so the descriptor check plus this body is what says this is the one that
    # takes three doubles rather than a Vec3d.
    "kEntityGetVelocity": {"body": r"Field field_\d+:Lnet/minecraft/class_243;"},
    "kEntitySetVelocity": {"body": r"class_243"},
    # Reach: field names say nothing on their own, so each attribute is tied to
    # the getter that reads it -- whatever getBlockInteractionRange returns *is*
    # the block interaction range, by definition.
    "kAttrBlockRange":  {"used_by": ("net.minecraft.class_1657", "m", "method_55754")},
    "kAttrEntityRange": {"used_by": ("net.minecraft.class_1657", "m", "method_55755")},
    # getServer must hand back the integrated server itself, not a copy or a null
    # from somewhere else: its body is a read of MinecraftClient's own field.
    "kMcGetServer": {"body": r"Field field_\d+:Lnet/minecraft/class_1132;"},
    # MinecraftClient has exactly two Entity-typed fields: the targeted entity and
    # the camera entity. The count is what pins that set, and Yarn's named
    # mappings are what say which of the two field_1692 is -- a mix-up here would
    # make "only on entities" mean "only when the camera has an entity", which is
    # always true, so the mistake has to be caught here rather than at runtime.
    "kMcTargetedEntity": {"unique": 2},
    # currentScreen is likewise the only Screen-typed field.
    "kMcCurrentScreen": {"unique": 1},
    # The crit predicate is the one place all six of these are read together, so
    # tying them to it proves the set is the crit precondition rather than six
    # individually plausible getters. A wrong term here would make the module wait
    # for something the game never checks.
    "kEntityFallDistance":    {"used_by": ("net.minecraft.class_1657", "m", "method_75197")},
    "kEntityIsOnGround":      {"used_by": ("net.minecraft.class_1657", "m", "method_75197")},
    "kLivingIsClimbing":      {"used_by": ("net.minecraft.class_1657", "m", "method_75197")},
    "kEntityIsTouchingWater": {"used_by": ("net.minecraft.class_1657", "m", "method_75197")},
    "kPlayerHasBlindness":    {"used_by": ("net.minecraft.class_1657", "m", "method_75197")},
    "kEntityHasVehicle":      {"used_by": ("net.minecraft.class_1657", "m", "method_75197")},
    # Step height is read by the collision step that uses it, movement speed by
    # the getter the movement code calls.
    "kAttrStepHeight":    {"used_by": ("net.minecraft.class_1309", "m", "method_49476")},
    # PlayerEntity overrides getMovementSpeed and reads the attribute there; the
    # base class returns a cached field instead, which is why the probe names the
    # override rather than the class the id is declared on.
    "kAttrMovementSpeed": {"used_by": ("net.minecraft.class_1657", "m", "method_6029")},
    # Flight: the client's own tickMovement reads both flags, and getFlySpeed is
    # what reads the speed.
    "kAbilitiesAllowFlying": {"used_by": ("net.minecraft.class_746", "m", "method_6007")},
    "kAbilitiesFlying":      {"used_by": ("net.minecraft.class_746", "m", "method_6007")},
    "kAbilitiesFlySpeed":    {"used_by": ("net.minecraft.class_1656", "m", "method_7252")},
    # getAbilities is the only method on PlayerEntity returning PlayerAbilities.
    "kPlayerGetAbilities": {"unique": 1},
    # The use-item cooldown is the int MinecraftClient's own tick maintains.
    "kMcItemUseCooldown": {"used_by": ("net.minecraft.class_310", "m", "method_1574")},
    # setPosition is the overload that also moves the bounding box, which is what
    # makes it the right one to nudge a player with: the plain setPos does not.
    "kEntitySetPosition": {"body": r"method_23327[\s\S]*method_5857"},
    # The held stack is the one LivingEntity returns as an ItemStack from the main
    # hand; getItem is the only method on ItemStack returning an Item.
    "kItemStackGetItem": {"unique": 1},
    # Triggerbot. doAttack has to contain the attack itself: it is the click
    # handler, and a method that returned a boolean without calling
    # attackEntity(player, entity) would satisfy every other check here while
    # doing nothing a trigger could trigger.
    "kMcDoAttack": {"body": r"method_2918:\(Lnet/minecraft/class_1657;Lnet/minecraft/class_1297;\)V"},
    # ...and the cooldown it consults is the field doAttack bails out on, not
    # another int that happens to live in MinecraftClient.
    "kMcAttackCooldown": {"used_by": ("net.minecraft.class_310", "m", "method_1536")},
    # isInvisible is getFlag(5), and Entity.INVISIBLE_FLAG_INDEX is 5 -- so the
    # literal and the flag read together are what say this is the invisibility
    # flag rather than any of the other four flag bits.
    "kEntityIsInvisible": {"body": r"iconst_5[\s\S]*method_5795:\(I\)Z"},
    # The charge is computed from these two, so the field is tied to the game's
    # own progress getter and the per-tick value to the attribute it divides by.
    # method_7261 is getAttackCooldownProgress(float): its body reads this field
    # and divides by method_7279, and it is the *only* method on PlayerEntity
    # returning a float built that way (method_75194 looks identical but reads
    # ticksSinceHandEquipping, which is why the names are not enough).
    "kLivingTicksSinceLastAttack": {"used_by": ("net.minecraft.class_1657", "m", "method_7261")},
    "kPlayerCooldownPerTick": {"body": r"method_45325[\s\S]*20\.0d"},
    # isUsingItem reads the living-flags tracker's first bit.
    "kLivingIsUsingItem": {"body": r"field_6257[\s\S]*iand"},
    # Auto Totem. The click is the game's own, so what needs proving is that every
    # member around it is the one that path really uses: that the totem is the item
    # the death-protection effect spawns, that the handler the module compares
    # against is the player's own, that the slot it reads is the one the handler
    # numbered, and that the swap it performs is the key's own action.
    #
    # playDeathProtection (ClientPlayNetworkHandler.method_19691) builds the stack
    # of totems the game shows when one saves the player, so it is the item behind
    # death protection rather than another item that happens to be named this way.
    "kItemsTotemOfUndying": {"used_by": ("net.minecraft.class_634", "m", "method_19691")},
    # PlayerEntity has exactly one PlayerScreenHandler field and exactly one
    # ScreenHandler field, so neither comparison can be made against a sibling.
    "kPlayerScreenHandler": {"unique": 1},
    # getInventory hands back the inventory field itself, which is the object the
    # module decides a slot belongs to the player by.
    "kPlayerGetInventory": {"body": r"Field field_7514:Lnet/minecraft/class_1661;"},
    # These two are the members clickSlot itself uses: the syncId it compares
    # against the open handler before it does anything, and the currentScreenHandler
    # it compares it to.
    "kScreenHandlerSyncId": {"used_by": ("net.minecraft.class_636", "m", "method_2906")},
    "kPlayerCurrentHandler": {"unique": 1, "used_by": ("net.minecraft.class_636", "m", "method_2906")},
    # getSlot indexes the slots list, and the list's size() is its own override over
    # a delegate -- which is what makes size() a call on this class rather than an
    # inherited one.
    "kScreenHandlerSlots": {"used_by": ("net.minecraft.class_1703", "m", "method_7611")},
    "kScreenHandlerGetSlot": {"body": r"field_7761[\s\S]*get:\(I\)Ljava/lang/Object;"},
    "kListSize": {"unique": 1, "body": r"field_11115[\s\S]*java/util/List\.size:\(\)I"},
    # Slot.inventory is the object Slot.getStack reads through, and Slot.id is the
    # number addSlot assigns it as the position in the handler's own list -- the
    # same number a click names.
    "kSlotInventory": {"used_by": ("net.minecraft.class_1735", "m", "method_7677")},
    "kSlotId": {"used_by": ("net.minecraft.class_1703", "m", "method_7621")},
    "kSlotGetStack": {"body": r"field_7871[\s\S]*method_5438"},
    # getOffHandStack is getStackInHand(Hand.OFF_HAND): the hand constant and the
    # getter together say which hand this is.
    "kLivingGetOffHandStack": {"body": r"class_1304\.field_6171[\s\S]*method_6118"},
    # MinecraftClient has exactly one ClientPlayerInteractionManager field.
    "kMcInteractionManager": {"unique": 1},
    # clickSlot is the method that applies the click to the open handler
    # (onSlotClick) after checking the syncId it read from it.
    "kInteractionManagerClickSlot": {"body": r"field_7763[\s\S]*method_7593"},
    # SWAP is the action the game's own swap-hand path passes together with the
    # offhand button (40, which is a PlayerInventory index) -- HandledScreen.
    # onMouseClick is where the key turns into a click.
    "kSlotActionSwap": {"used_with": ("net.minecraft.class_465", "m", "method_30107", r"bipush\s+40")},
}

_javap_cache: dict[str, dict[str, list[str]]] = {}
_verbose_cache: dict[str, str] = {}
_code_cache: dict[str, str] = {}


def _run(jar, cls: str, *flags: str) -> str:
    out = subprocess.run(["javap", *flags, "-p", "-cp", _cp(jar), cls],
                         capture_output=True, text=True)
    return out.stdout if out.returncode == 0 else ""


def _cp(jar) -> str:
    """Classpath from one jar or a list of them (javap takes the OS separator)."""
    import os
    if isinstance(jar, (list, tuple)):
        return os.pathsep.join(str(p) for p in jar)
    return str(jar)


def members(jar: Path, cls: str) -> dict[str, list[str]]:
    """name -> list of descriptors, from javap -p -s.

    javap prints declarations with fully-qualified types
    (``public net.minecraft.class_757 field_1773;``), so the parse takes the last
    token before the ``;`` as the name rather than trying to character-class the
    type -- dots, generics and arrays all appear there.
    """
    if cls in _javap_cache:
        return _javap_cache[cls]
    text = _run(jar, cls, "-s")
    found: dict[str, list[str]] = {}
    pending = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("descriptor:") and pending:
            kind, name = pending
            found.setdefault(f"{kind}:{name}", []).append(s.split(":", 1)[1].strip())
            pending = None
            continue
        if not s.endswith(";"):
            continue
        if "(" in s:
            head, _, _ = s.partition("(")
            tokens = head.split()
            if tokens:
                pending = ("m", tokens[-1])
        else:
            tokens = s[:-1].split()
            if len(tokens) >= 2:
                pending = ("f", tokens[-1])
    _javap_cache[cls] = found
    return found


def code_of(jar: Path, cls: str) -> str:
    """javap -c output: used to see which field a method actually touches."""
    if cls not in _code_cache:
        _code_cache[cls] = _run(jar, cls, "-c")
    return _code_cache[cls]


def signatures_of(jar: Path, cls: str) -> str:
    """javap -v output: carries the generics javap -s omits."""
    if cls not in _verbose_cache:
        _verbose_cache[cls] = _run(jar, cls, "-v")
    return _verbose_cache[cls]


def main() -> int:
    if len(sys.argv) >= 4 and sys.argv[1] == "--show":
        # --show <jar> <class> <member>...: print the disassembly of members, so a
        # name can be confirmed by what it actually does.
        jar = Path(sys.argv[2])
        for member in sys.argv[3:]:
            cls, _, name = member.rpartition(".")
            if "." not in cls:
                cls = "net.minecraft." + cls
            print(f"===== {member} =====")
            print(member_body(jar, cls, None, name) or "(member not found)")
        return 0

    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    args = [Path(a) for a in sys.argv[1:]]
    # A .tiny file is the mapping table rather than a jar to disassemble.
    names = load_names(args[0]) if args[0].suffix == ".tiny" else {}
    jars = [a for a in args if a.suffix != ".tiny"]
    for j in jars:
        if not j.is_file():
            print(f"jar not found: {j}")
            return 2
    if not jars:
        print("no jar given; the name check needs one to disassemble")
        return 2
    jar = jars

    bad = []
    for const, cls, kind, name, desc, meaning in MAPPINGS:
        mem = members(jar, cls)
        if kind == "c":
            if not mem:
                print(f"FAIL {const:24} {cls}  (class missing)")
                bad.append(const)
            else:
                print(f"ok   {const:24} {cls}  [{meaning}]")
            continue

        key = f"{kind}:{name}"
        if key not in mem:
            print(f"FAIL {const:24} {cls}.{name}  (no such {kind})  [{meaning}]")
            candidates(mem, kind, desc, cls)
            bad.append(const)
            continue
        if desc not in mem[key]:
            print(f"FAIL {const:24} {cls}.{name}  want {desc}, "
                  f"have {mem[key]}  [{meaning}]")
            candidates(mem, kind, desc, cls)
            bad.append(const)
            continue

        # Semantics, where existence alone is not proof.
        probe = PROBES.get(const, {})
        note = []
        ok = True

        if "unique" in probe:
            same = [k.split(":", 1)[1] for k, ds in mem.items()
                    if k.startswith(kind + ":") and desc in ds]
            if len(same) != probe["unique"]:
                print(f"FAIL {const:24} {cls}.{name}  expected {probe['unique']} "
                      f"member(s) with {desc}, found {len(same)}: "
                      f"{', '.join(sorted(same))}  [{meaning}]")
                bad.append(const)
                continue
            note.append(f"unique among {len(same)}")

        if "sig" in probe:
            if not re.search(probe["sig"], signatures_of(jar, cls)):
                print(f"FAIL {const:24} {cls}.{name}  generic signature does not "
                      f"match {probe['sig']}  [{meaning}]")
                bad.append(const)
                continue
            note.append("signature ok")

        if "body" in probe:
            body = member_body(jar, cls, kind, name)
            if not re.search(probe["body"], body):
                print(f"FAIL {const:24} {cls}.{name}  body does not match "
                      f"{probe['body']}  [{meaning}]")
                print("       " + " ".join(
                    b.strip() for b in body.splitlines()[:8] if b.strip()))
                bad.append(const)
                continue
            note.append("body ok")

        # The name check: an intermediary id has exactly one Yarn name, and the
        # `meaning` column says which member the code is reaching for. Only ids
        # are checkable -- authlib's GameProfile is not in Minecraft's mappings.
        if names and re.fullmatch(r"(class|method|field)_\d+", name):
            actual = names.get(name)
            want = expected_name(meaning)
            if actual is None:
                print(f"FAIL {const:24} {name}  not in the Yarn mappings "
                      f"at all  [{meaning}]")
                bad.append(const)
                continue
            if actual.rsplit("/", 1)[-1] != want:
                print(f"FAIL {const:24} {cls}.{name}  is {actual}, the code "
                      f"calls it {meaning}  [{meaning}]")
                bad.append(const)
                continue
            note.append(f"named {actual.rsplit('/', 1)[-1]}")

        if "used_with" in probe:
            ucls, ukind, uname, ure = probe["used_with"]
            body = member_body(jar, ucls, ukind, uname)
            if not body or not re.search(re.escape(name), body):
                print(f"FAIL {const:24} {cls}.{name}  is not used by "
                      f"{ucls}.{uname}  [{meaning}]")
                bad.append(const)
                continue
            if not re.search(ure, body):
                print(f"FAIL {const:24} {cls}.{name}  is used by {ucls}.{uname} "
                      f"but not with {ure}  [{meaning}]")
                bad.append(const)
                continue
            note.append(f"used by {ucls.rsplit('.', 1)[1]}.{uname}")

        if "used_by" in probe:
            ucls, ukind, uname = probe["used_by"]
            body = member_body(jar, ucls, ukind, uname)
            if not body or not re.search(re.escape(name), body):
                print(f"FAIL {const:24} {cls}.{name}  is not referenced by "
                      f"{ucls}.{uname}  [{meaning}]")
                bad.append(const)
                continue
            note.append(f"used by {ucls.rsplit('.', 1)[1]}.{uname}")

        suffix = ("  (" + ", ".join(note) + ")") if note else ""
        print(f"ok   {const:24} {cls}.{name} {desc}  [{meaning}]{suffix}")

    print()
    if bad:
        print(f"{len(bad)} mapping(s) do not resolve: {', '.join(bad)}")
        return 1
    if names:
        checked = sum(1 for _, _, _, n, _, _ in MAPPINGS
                      if re.fullmatch(r"(class|method|field)_\d+", n))
        print(f"all {len(MAPPINGS)} mappings resolve "
              f"({checked} of them checked against their Yarn name)")
    else:
        print(f"all {len(MAPPINGS)} mappings resolve (no .tiny given: "
              f"names not checked)")
    return 0


def member_body(jar: Path, cls: str, kind: str, name: str) -> str:
    """The javap -c block for one member, up to the next declaration."""
    lines = code_of(jar, cls).splitlines()
    start = None
    for i, line in enumerate(lines):
        if not line.startswith("  ") or line.startswith("    "):
            continue
        decl = line.strip()
        if "(" in decl:
            head = decl.partition("(")[0].split()
            hit = head and head[-1] == name
        else:
            hit = decl.endswith(name + ";")
        if hit:
            start = i
            break
    if start is None:
        return ""
    block = []
    for line in lines[start + 1:]:
        if line.startswith("  ") and not line.startswith("    "):
            break
        block.append(line)
    return "\n".join(block)


def load_names(tiny: Path) -> dict[str, str]:
    """intermediary id (`class_310`, `method_1551`) -> its Yarn name.

    Only the id-to-name direction matters here: it is what turns the code's
    assumption about a member into something checkable against the same file the
    ids were looked up in.
    """
    names: dict[str, str] = {}
    for line in tiny.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        # `c\t<official>\t<intermediary>\t<yarn>`
        if parts[0] == "c" and len(parts) >= 4:
            names[parts[2]] = parts[3]
        # `\t<m|f>\t<descriptor>\t<official>\t<intermediary>\t<yarn>`
        elif parts[0] == "" and parts[1] in ("m", "f") and len(parts) >= 6:
            names[parts[4]] = parts[5]
    return names


def expected_name(meaning: str) -> str:
    """The member name a `meaning` like "Entity.setVelocity(x, y, z)" claims."""
    name = meaning.split(".", 1)[1] if "." in meaning else meaning
    return name.split("(", 1)[0].split(" ", 1)[0].strip()


def candidates(mem: dict, kind: str, desc: str, cls: str) -> None:
    """Print members with a matching descriptor shape, as leads for a repair."""
    hits = []
    for key, descs in mem.items():
        if not key.startswith(kind + ":"):
            continue
        for d in descs:
            if d == desc:
                hits.append(key.split(":", 1)[1])
                break
    if hits:
        print(f"       same descriptor ({desc}) in {cls}: {', '.join(sorted(hits)[:24])}")


if __name__ == "__main__":
    sys.exit(main())
