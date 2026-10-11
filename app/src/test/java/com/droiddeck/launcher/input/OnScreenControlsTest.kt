package com.droiddeck.launcher.input

import kotlin.math.abs
import kotlin.math.sqrt
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout
import java.io.File
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE, qualifiers = "mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class OnScreenControlsTest {
    private lateinit var context: Context
    private lateinit var view: OnScreenControls
    private var time = 1000L

    @Before fun setUp() {
        context = RuntimeEnvironment.getApplication()
        context.getSharedPreferences("controller", Context.MODE_PRIVATE).edit().clear().commit()
        ControllerPrefs.setLayout(context, 1200, 800, mapOf("ls" to (0.45f to 0.5f), "rs" to (0.55f to 0.5f)))
        view = OnScreenControls(context, null)
        view.layout(0, 0, 1200, 800)
    }

    @Test fun editorSnapsToOtherGroupCentreLines() {
        editor()
        val button = control("lb")
        val target = control("rb")
        val x = value(button, "cx")
        val y = value(button, "cy")
        touch(MotionEvent.ACTION_DOWN, 1 to (x to y))
        touch(MotionEvent.ACTION_MOVE, 1 to (x to y + 5f))
        assertEquals(value(target, "cy"), value(button, "cy"), 0f)
        assertEquals(value(target, "cy"), nullableFloat(view, "snapY")!!, 0f)
        touch(MotionEvent.ACTION_UP, 1 to (x to y + 5f))
        assertNull(nullableFloat(view, "snapY"))
    }

    @Test fun editorSnapsToClusterCentresNotTheirOuterButtons() {
        editor()
        val button = control("lb")
        val x = value(button, "cx")
        val row = value(control("y"), "cy") + 3f
        touch(MotionEvent.ACTION_DOWN, 1 to (x to value(button, "cy")))
        touch(MotionEvent.ACTION_MOVE, 1 to (x to row))
        assertEquals(row, value(button, "cy"), 0f)
        assertNull(nullableFloat(view, "snapY"))
        touch(MotionEvent.ACTION_MOVE, 1 to (x to value(control("x"), "cy") + 3f))
        assertEquals(value(control("x"), "cy"), value(button, "cy"), 0f)
        touch(MotionEvent.ACTION_UP, 1 to (x to row))
    }

    @Test fun editorDoesNotSnapBeyondDistanceAndCanDragFreeFromGuide() {
        editor()
        val button = control("lb")
        val x = value(button, "cx")
        val y = value(button, "cy")
        val otherYs = (field(view, "controls") as List<*>)
            .filter { field(it!!, "group") != "lb" }
            .map { value(it!!, "cy") }
        val freeY = (100..700 step 31).map(Int::toFloat).first {
            otherYs.minOf { targetY -> abs(it - targetY) } > 12f
        }
        touch(MotionEvent.ACTION_DOWN, 1 to (x to y))
        touch(MotionEvent.ACTION_MOVE, 1 to (x to freeY))
        assertEquals(freeY, value(button, "cy"), 0f)
        assertNull(nullableFloat(view, "snapY"))
        val target = value(control("rb"), "cy")
        touch(MotionEvent.ACTION_MOVE, 1 to (x to target + 5f))
        assertEquals(target, value(button, "cy"), 0f)
        touch(MotionEvent.ACTION_MOVE, 1 to (x to freeY))
        assertEquals(freeY, value(button, "cy"), 0f)
        assertNull(nullableFloat(view, "snapY"))
        touch(MotionEvent.ACTION_UP, 1 to (x to freeY))
    }

    @Test fun editorSnapsToScreenCentreAndCanDisableSnapping() {
        editor()
        val button = control("lb")
        val y = value(button, "cy")
        touch(MotionEvent.ACTION_DOWN, 1 to (value(button, "cx") to y))
        touch(MotionEvent.ACTION_MOVE, 1 to (605f to y))
        assertEquals(600f, value(button, "cx"), 0f)
        assertEquals(600f, nullableFloat(view, "snapX")!!, 0f)
        touch(MotionEvent.ACTION_UP, 1 to (605f to y))

        ControllerPrefs.setSnap(context, false)
        editor()
        val unsnapped = control("lb")
        val x = value(unsnapped, "cx")
        val startY = value(unsnapped, "cy")
        touch(MotionEvent.ACTION_DOWN, 1 to (x to startY))
        touch(MotionEvent.ACTION_MOVE, 1 to (605f to startY))
        assertEquals(605f, value(unsnapped, "cx"), 0f)
        assertNull(nullableFloat(view, "snapX"))
        touch(MotionEvent.ACTION_UP, 1 to (605f to startY))
    }

    @Test fun editorSnapPreferenceDefaultsOnAndCanBeChanged() {
        assertTrue(ControllerPrefs.read(context).snap)
        ControllerPrefs.setSnap(context, false)
        assertFalse(ControllerPrefs.read(context).snap)
        ControllerPrefs.resetAll(context)
        assertTrue(ControllerPrefs.read(context).snap)
    }

    @Test fun adaptiveStickHidesUntilTouchedAndReturnsToNeutralOnRelease() {
        val left = control("ls")
        assertEquals(0, pixel(value(left, "cx"), value(left, "cy")))
        assertTrue(touch(MotionEvent.ACTION_DOWN, 7 to (540f to 400f)))
        assertEquals(7, field(left, "pressedBy"))
        assertEquals(540f, value(left, "ax"), 0f)
        assertEquals(400f, value(left, "ay"), 0f)
        assertNotEquals(0, pixel(540f, 400f))
        touch(MotionEvent.ACTION_MOVE, 7 to (580f to 420f))
        assertTrue(value(left, "kx") > 0f)
        assertTrue(value(left, "ky") > 0f)
        touch(MotionEvent.ACTION_UP, 7 to (580f to 420f))
        assertEquals(-1, field(left, "pressedBy"))
        assertEquals(0f, value(left, "kx"), 0f)
        assertEquals(0f, value(left, "ky"), 0f)
        assertEquals(0, pixel(540f, 400f))
    }

    @Test fun simultaneousSticksKeepTheirPointersAndReleaseIndependently() {
        val left = control("ls")
        val right = control("rs")
        touch(MotionEvent.ACTION_DOWN, 7 to (540f to 400f))
        touch(MotionEvent.ACTION_POINTER_DOWN or (1 shl 8), 7 to (540f to 400f), 9 to (660f to 400f))
        assertEquals(7, field(left, "pressedBy"))
        assertEquals(9, field(right, "pressedBy"))
        touch(MotionEvent.ACTION_POINTER_DOWN or (2 shl 8), 7 to (540f to 400f), 9 to (660f to 400f), 11 to (500f to 400f))
        assertEquals(7, field(left, "pressedBy"))
        touch(MotionEvent.ACTION_POINTER_UP or (2 shl 8), 7 to (540f to 400f), 9 to (660f to 400f), 11 to (500f to 400f))
        touch(MotionEvent.ACTION_MOVE, 7 to (700f to 400f), 9 to (500f to 400f))
        assertTrue(value(left, "kx") > 0f)
        assertTrue(value(right, "kx") < 0f)
        touch(MotionEvent.ACTION_POINTER_UP, 7 to (700f to 400f), 9 to (500f to 400f))
        assertEquals(-1, field(left, "pressedBy"))
        assertEquals(9, field(right, "pressedBy"))
        touch(MotionEvent.ACTION_CANCEL, 9 to (500f to 400f))
        for (stick in listOf(left, right)) {
            assertEquals(-1, field(stick, "pressedBy"))
            assertEquals(0f, value(stick, "kx"), 0f)
        }
    }

    @Test fun cancelledTouchDoesNotBecomeAStickClick() {
        touch(MotionEvent.ACTION_DOWN, 1 to (540f to 400f))
        touch(MotionEvent.ACTION_CANCEL, 1 to (540f to 400f))
        touch(MotionEvent.ACTION_DOWN, 1 to (540f to 400f))
        assertEquals(false, field(control("ls"), "clicked"))
        touch(MotionEvent.ACTION_UP, 1 to (540f to 400f))
        touch(MotionEvent.ACTION_DOWN, 1 to (540f to 400f))
        assertEquals(true, field(control("ls"), "clicked"))
    }

    @Test @Config(shadows = [ClosedInputWriter::class])
    fun separateButtonsKeepStickClicksAvailableWithoutDoubleTapClicks() {
        ControllerPrefs.setStickClick(context, false)
        val bridge = PadBridge(File(context.cacheDir, "stick-click-test"))
        val state = field(bridge, "state") as PadState
        view = OnScreenControls(context, bridge)
        view.layout(0, 0, 1200, 800)
        for ((id, button) in listOf("l3" to PadState.L3, "r3" to PadState.R3)) {
            val click = control(id)
            val position = value(click, "cx") to value(click, "cy")
            assertNotEquals(0, pixel(position.first, position.second))
            assertTrue(touch(MotionEvent.ACTION_DOWN, 1 to position))
            assertTrue(state.isDown(button))
            touch(MotionEvent.ACTION_UP, 1 to position)
            assertFalse(state.isDown(button))
        }
        // Lifting and re-grabbing the camera stick must not click R3 in this mode.
        repeat(2) {
            touch(MotionEvent.ACTION_DOWN, 1 to (660f to 400f))
            touch(MotionEvent.ACTION_MOVE, 1 to (680f to 400f))
            assertTrue(state.rightX > 0f)
            assertFalse(state.isDown(PadState.R3))
            touch(MotionEvent.ACTION_UP, 1 to (680f to 400f))
        }
        bridge.stop()
    }

    @Test @Config(shadows = [ClosedInputWriter::class])
    fun separateStickClickCanBeHeldAlongsideCameraAndReleasesOnCancelOrModeChange() {
        ControllerPrefs.setStickClick(context, false)
        val bridge = PadBridge(File(context.cacheDir, "stick-click-test"))
        val state = field(bridge, "state") as PadState
        view = OnScreenControls(context, bridge)
        view.layout(0, 0, 1200, 800)
        val click = control("r3")
        val position = value(click, "cx") to value(click, "cy")
        touch(MotionEvent.ACTION_DOWN, 1 to (660f to 400f))
        touch(MotionEvent.ACTION_MOVE, 1 to (680f to 400f))
        touch(MotionEvent.ACTION_POINTER_DOWN or (1 shl 8), 1 to (680f to 400f), 2 to position)
        assertTrue(state.rightX > 0f)
        assertTrue(state.isDown(PadState.R3))
        touch(MotionEvent.ACTION_CANCEL, 1 to (680f to 400f), 2 to position)
        assertEquals(0f, state.rightX, 0f)
        assertFalse(state.isDown(PadState.R3))
        touch(MotionEvent.ACTION_DOWN, 1 to position)
        assertTrue(state.isDown(PadState.R3))
        ControllerPrefs.setStickClick(context, true)
        view.reload()
        assertFalse(state.isDown(PadState.R3))
        assertEquals(0, pixel(position.first, position.second))
        assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to position))
        bridge.stop()
    }

    @Test fun separateStickClickButtonsRespectSavedLayoutAndOverlayVisibility() {
        ControllerPrefs.setStickClick(context, false)
        ControllerPrefs.setLayout(context, 1200, 800, mapOf("l3" to (0.4f to 0.2f), "r3" to (0.6f to 0.2f)))
        view.reload()
        assertEquals(480f, value(control("l3"), "cx"), 0f)
        assertEquals(720f, value(control("r3"), "cx"), 0f)
        assertTrue(touch(MotionEvent.ACTION_DOWN, 1 to (480f to 160f)))
        assertEquals(1, field(control("l3"), "pressedBy"))
        view.setQuickHidden(true)
        assertEquals(-1, field(control("l3"), "pressedBy"))
        assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (480f to 160f)))
        view.setButtonsOnly(true)
        assertEquals(0, pixel(720f, 160f))
        assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (720f to 160f)))
    }

    @Test fun newStickClickButtonsAvoidControlsInLegacyCustomLayout() {
        assertNewClicksAvoidLegacyLayout(1200, 800)
    }

    @Test @Config(qualifiers = "xxhdpi")
    fun newStickClickButtonsAvoidLegacyLayoutOnHighDensityLandscapeScreen() {
        assertNewClicksAvoidLegacyLayout(2992, 1344)
    }

    private fun assertNewClicksAvoidLegacyLayout(width: Int, height: Int) {
        ControllerPrefs.resetAllLayouts(context)
        ControllerPrefs.setStickClick(context, false)
        view = OnScreenControls(context, null)
        view.layout(0, 0, width, height)
        // An older layout has no L3/R3 coordinates. Its saved buttons may occupy the
        // spaces where the new buttons would have been placed in the automatic layout.
        val legacy = listOf("select" to "l3", "start" to "r3").associate { (oldId, newId) ->
            oldId to (value(control(newId), "cx") / width to value(control(newId), "cy") / height)
        }
        ControllerPrefs.setLayout(context, width, height, legacy)
        view.reload()
        for ((id, position) in legacy) {
            assertEquals(position.first * width, value(control(id), "cx"), 0.01f)
            assertEquals(position.second * height, value(control(id), "cy"), 0.01f)
        }
        for (id in listOf("l3", "r3")) {
            val button = control(id)
            val x = value(button, "cx")
            val y = value(button, "cy")
            val edge = value(button, "radius") * 1.2f
            for (position in listOf(x to y, x - edge to y, x + edge to y, x to y - edge, x to y + edge)) {
                assertTrue(touch(MotionEvent.ACTION_DOWN, 3 to position))
                assertEquals("$id must own its hit target in the upgraded layout", 3, field(button, "pressedBy"))
                touch(MotionEvent.ACTION_UP, 3 to position)
            }
        }
    }

    @Test fun buttonsTakePriorityOverAdaptiveRegions() {
        val button = control("a")
        touch(MotionEvent.ACTION_DOWN, 3 to (value(button, "cx") to value(button, "cy")))
        assertEquals(3, field(button, "pressedBy"))
        assertEquals(-1, field(control("rs"), "pressedBy"))
    }

    @Test fun fixedSticksAndEditorRemainVisibleAtSavedPositions() {
        ControllerPrefs.setAdaptiveSticks(context, false)
        view.reload()
        val left = control("ls")
        val x = value(left, "cx")
        val y = value(left, "cy")
        assertNotEquals(0, pixel(x, y))
        assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (600f to 200f)))
        touch(MotionEvent.ACTION_DOWN, 1 to (x + 10f to y))
        assertEquals(x, value(left, "ax"), 0f)
        assertEquals(10f, value(left, "kx"), 0f)
        ControllerPrefs.setAdaptiveSticks(context, true)
        view = OnScreenControls(context, null, editing = true)
        view.layout(0, 0, 1200, 800)
        assertNotEquals(0, pixel(value(control("ls"), "cx"), value(control("ls"), "cy")))
    }

    @Test fun buttonsOnlyAndReloadReleaseAdaptiveSticks() {
        touch(MotionEvent.ACTION_DOWN, 1 to (540f to 400f))
        touch(MotionEvent.ACTION_MOVE, 1 to (570f to 400f))
        view.reload()
        assertEquals(-1, field(control("ls"), "pressedBy"))
        assertEquals(0f, value(control("ls"), "kx"), 0f)
        view.setButtonsOnly(true)
        assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (600f to 200f)))
    }

    @Test fun adaptiveActivationIsLimitedToExpandedSavedCircle() {
        val left = control("ls")
        val x = value(left, "cx")
        val y = value(left, "cy")
        val reach = value(left, "radius") * 1.4f * sqrt(1.5f)
        assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (x to y - reach - 1f)))
        assertTrue(touch(MotionEvent.ACTION_DOWN, 1 to (x to y - reach + 1f)))
        assertEquals(y - reach + 1f, value(left, "ay"), 0.001f)
        touch(MotionEvent.ACTION_UP, 1 to (x to y - reach + 1f))
        assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (x - reach to y - reach)))
        assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (600f to 200f)))
    }

    @Test fun touchesOutsideActivationAreasStayWithTrackpad() {
        val received = mutableListOf<Int>()
        val trackpad = View(context).apply {
            setOnTouchListener { _, event -> received.add(event.actionMasked); true }
        }
        val root = FrameLayout(context).apply {
            addView(trackpad, FrameLayout.LayoutParams(1200, 800))
            addView(view, FrameLayout.LayoutParams(1200, 800))
            measure(View.MeasureSpec.makeMeasureSpec(1200, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(800, View.MeasureSpec.EXACTLY))
            layout(0, 0, 1200, 800)
        }
        for ((action, position) in listOf(
            MotionEvent.ACTION_DOWN to (600f to 200f),
            MotionEvent.ACTION_MOVE to (540f to 400f),
            MotionEvent.ACTION_UP to (540f to 400f),
        )) {
            val event = MotionEvent.obtain(1000L, time++, action, position.first, position.second, 0)
            try { assertTrue(root.dispatchTouchEvent(event)) } finally { event.recycle() }
        }
        assertEquals(listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE, MotionEvent.ACTION_UP), received)
        assertEquals(-1, field(control("ls"), "pressedBy"))
    }

    @Test @Config(shadows = [ClosedInputWriter::class])
    fun eachStickAloneDoesNotForwardGuestEdgesOrPressAttackButtons() {
        val guest = mutableListOf<Int>()
        val bridge = PadBridge(File(context.cacheDir, "single-stick-routing-test"))
        val state = field(bridge, "state") as PadState
        view = OnScreenControls(context, bridge, onGuestTouch = { event -> guest.add(event.actionMasked); true })
        view.layout(0, 0, 1200, 800)
        for (id in listOf("ls", "rs")) {
            val stick = control(id)
            val x = value(stick, "cx")
            val y = value(stick, "cy")
            for ((action, position) in listOf(
                MotionEvent.ACTION_DOWN to (x to y),
                MotionEvent.ACTION_MOVE to (x + 25f to y),
                MotionEvent.ACTION_UP to (x + 25f to y),
            )) {
                assertTrue(touch(action, 7 to position))
                assertFalse("$id emitted an RB attack at action $action", state.isDown(PadState.RB))
                assertEquals("$id emitted an RT attack at action $action", 0f, state.rightTrigger, 0f)
                assertTrue("$id forwarded a guest touch at action $action", guest.isEmpty())
                if (action == MotionEvent.ACTION_MOVE) assertTrue(if (id == "ls") state.leftX > 0f else state.rightX > 0f)
            }
            assertEquals(0f, state.leftX, 0f)
            assertEquals(0f, state.rightX, 0f)
        }
        bridge.stop()
    }

    @Test @Config(shadows = [ClosedInputWriter::class])
    fun stickGesturesAndExtraFingerOnHeldStickNeverReachGuestOrAttackButtons() {
        val guest = mutableListOf<List<Int>>()
        val bridge = PadBridge(File(context.cacheDir, "stick-routing-test"))
        val state = field(bridge, "state") as PadState
        view = OnScreenControls(context, bridge, onGuestTouch = { event ->
            guest.add((0 until event.pointerCount).map(event::getPointerId)); true
        })
        view.layout(0, 0, 1200, 800)
        val attack = control("rb")
        val attackPosition = value(attack, "cx") to value(attack, "cy")
        touch(MotionEvent.ACTION_DOWN, 7 to (540f to 400f))
        touch(MotionEvent.ACTION_POINTER_DOWN or (1 shl 8), 7 to (540f to 400f), 9 to (660f to 400f))
        touch(MotionEvent.ACTION_POINTER_DOWN or (2 shl 8), 7 to (540f to 400f), 9 to (660f to 400f), 11 to (540f to 400f))
        touch(MotionEvent.ACTION_MOVE, 7 to (565f to 400f), 9 to (635f to 400f), 11 to attackPosition)
        assertTrue(state.leftX > 0f)
        assertTrue(state.rightX < 0f)
        assertFalse(state.isDown(PadState.RB))
        assertEquals(0f, state.rightTrigger, 0f)
        touch(MotionEvent.ACTION_POINTER_UP or (2 shl 8), 7 to (565f to 400f), 9 to (635f to 400f), 11 to attackPosition)
        touch(MotionEvent.ACTION_POINTER_UP, 7 to (565f to 400f), 9 to (635f to 400f))
        assertEquals(0f, state.leftX, 0f)
        assertTrue(state.rightX < 0f)
        touch(MotionEvent.ACTION_UP, 9 to (635f to 400f))
        assertEquals(0f, state.rightX, 0f)
        assertFalse(state.isDown(PadState.RB))
        assertTrue("a joystick gesture must not produce any guest touch/mouse events", guest.isEmpty())
        bridge.stop()
    }

    @Test fun guestAndStickPointersStaySeparateRegardlessOfWhichTouchesFirst() {
        for (guestFirst in listOf(true, false)) {
            val guest = mutableListOf<Pair<Int, List<Int>>>()
            val receive: (MotionEvent) -> Boolean = { event ->
                guest.add(event.actionMasked to (0 until event.pointerCount).map(event::getPointerId)); true
            }
            view = OnScreenControls(context, null, onGuestTouch = receive)
            val root = FrameLayout(context).apply {
                addView(View(context).apply { setOnTouchListener { _, event -> receive(event) } }, FrameLayout.LayoutParams(1200, 800))
                addView(view, FrameLayout.LayoutParams(1200, 800))
                measure(View.MeasureSpec.makeMeasureSpec(1200, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(800, View.MeasureSpec.EXACTLY))
                layout(0, 0, 1200, 800)
            }
            val first = if (guestFirst) 11 to (600f to 200f) else 7 to (540f to 400f)
            val second = if (guestFirst) 7 to (540f to 400f) else 11 to (600f to 200f)
            dispatch(root, MotionEvent.ACTION_DOWN, first)
            dispatch(root, MotionEvent.ACTION_POINTER_DOWN or (1 shl 8), first, second)
            assertEquals(7, field(control("ls"), "pressedBy"))
            val attack = control("rb")
            val movedGuest = 11 to (value(attack, "cx") to value(attack, "cy"))
            val movedStick = 7 to (580f to 400f)
            val moved = if (guestFirst) arrayOf(movedGuest, movedStick) else arrayOf(movedStick, movedGuest)
            dispatch(root, MotionEvent.ACTION_MOVE, *moved)
            assertTrue(value(control("ls"), "kx") > 0f)
            assertEquals("a guest finger must not acquire the attack button", -1, field(attack, "pressedBy"))
            dispatch(root, MotionEvent.ACTION_POINTER_UP, *moved)
            dispatch(root, MotionEvent.ACTION_UP, moved[1])
            assertEquals(MotionEvent.ACTION_DOWN, guest.first().first)
            assertEquals(MotionEvent.ACTION_UP, guest.last().first)
            assertTrue(guest.all { it.second == listOf(11) })
            assertEquals(-1, field(control("ls"), "pressedBy"))
            view.visibility = View.GONE
            dispatch(root, MotionEvent.ACTION_DOWN, 17 to (540f to 400f))
            dispatch(root, MotionEvent.ACTION_UP, 17 to (540f to 400f))
            assertEquals(listOf(MotionEvent.ACTION_DOWN to listOf(17), MotionEvent.ACTION_UP to listOf(17)), guest.takeLast(2))
        }
    }

    @Test fun guestMultitouchActionsRemainBalancedWhileAStickIsHeld() {
        val guest = mutableListOf<Triple<Int, Int, List<Int>>>()
        view = OnScreenControls(context, null, onGuestTouch = { event ->
            guest.add(Triple(event.actionMasked, event.actionIndex, (0 until event.pointerCount).map(event::getPointerId))); true
        })
        view.layout(0, 0, 1200, 800)
        touch(MotionEvent.ACTION_DOWN, 7 to (540f to 400f))
        touch(MotionEvent.ACTION_POINTER_DOWN or (1 shl 8), 7 to (540f to 400f), 11 to (600f to 200f))
        touch(MotionEvent.ACTION_POINTER_DOWN or (2 shl 8), 7 to (540f to 400f), 11 to (600f to 200f), 13 to (650f to 200f))
        touch(MotionEvent.ACTION_POINTER_UP or (1 shl 8), 7 to (540f to 400f), 11 to (600f to 200f), 13 to (650f to 200f))
        touch(MotionEvent.ACTION_POINTER_UP or (1 shl 8), 7 to (540f to 400f), 13 to (650f to 200f))
        touch(MotionEvent.ACTION_UP, 7 to (540f to 400f))
        assertEquals(listOf(
            Triple(MotionEvent.ACTION_DOWN, 0, listOf(11)),
            Triple(MotionEvent.ACTION_POINTER_DOWN, 1, listOf(11, 13)),
            Triple(MotionEvent.ACTION_POINTER_UP, 0, listOf(11, 13)),
            Triple(MotionEvent.ACTION_UP, 0, listOf(13)),
        ), guest)
    }

    @Test fun hidingControlsCancelsOnlyGuestPointersAndDoesNotReassignHeldFingers() {
        val guest = mutableListOf<Pair<Int, List<Int>>>()
        view = OnScreenControls(context, null, onGuestTouch = { event ->
            guest.add(event.actionMasked to (0 until event.pointerCount).map(event::getPointerId)); true
        })
        view.layout(0, 0, 1200, 800)
        touch(MotionEvent.ACTION_DOWN, 7 to (540f to 400f))
        touch(MotionEvent.ACTION_POINTER_DOWN or (1 shl 8), 7 to (540f to 400f), 11 to (600f to 200f))
        view.setQuickHidden(true)
        touch(MotionEvent.ACTION_MOVE, 7 to (565f to 400f), 11 to (610f to 200f))
        touch(MotionEvent.ACTION_POINTER_UP, 7 to (565f to 400f), 11 to (610f to 200f))
        touch(MotionEvent.ACTION_UP, 11 to (610f to 200f))
        assertEquals(listOf(MotionEvent.ACTION_DOWN to listOf(11), MotionEvent.ACTION_CANCEL to listOf(11)), guest)
        assertEquals(-1, field(control("ls"), "pressedBy"))
        touch(MotionEvent.ACTION_DOWN, 17 to (540f to 400f))
        touch(MotionEvent.ACTION_UP, 17 to (540f to 400f))
        assertEquals(listOf(MotionEvent.ACTION_DOWN to listOf(17), MotionEvent.ACTION_UP to listOf(17)), guest.takeLast(2))
    }

    @Test fun reloadingDuringMixedGestureCancelsGuestAndReleasesStick() {
        val guest = mutableListOf<Pair<Int, List<Int>>>()
        view = OnScreenControls(context, null, onGuestTouch = { event ->
            guest.add(event.actionMasked to (0 until event.pointerCount).map(event::getPointerId)); true
        })
        view.layout(0, 0, 1200, 800)
        touch(MotionEvent.ACTION_DOWN, 7 to (540f to 400f))
        touch(MotionEvent.ACTION_POINTER_DOWN or (1 shl 8), 7 to (540f to 400f), 11 to (600f to 200f))
        view.reload()
        touch(MotionEvent.ACTION_POINTER_UP, 7 to (540f to 400f), 11 to (600f to 200f))
        touch(MotionEvent.ACTION_UP, 11 to (600f to 200f))
        assertEquals(listOf(MotionEvent.ACTION_DOWN to listOf(11), MotionEvent.ACTION_CANCEL to listOf(11)), guest)
        assertEquals(-1, field(control("ls"), "pressedBy"))
    }

    @Test fun adaptiveActivationLeavesFivePixelsAroundButtonHitTargets() {
        for (id in listOf("a", "rb")) {
            val button = control(id)
            val radius = value(button, "radius")
            val edge = value(button, "cy") - radius * if (id == "rb") 1.05f else 1.25f
            val x = value(button, "cx")
            val left = control("ls")
            left.javaClass.getDeclaredField("cx").apply { isAccessible = true }.setFloat(left, x)
            left.javaClass.getDeclaredField("cy").apply { isAccessible = true }.setFloat(left, edge - 10f)
            assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (x to edge - 4f)))
            assertEquals(-1, field(left, "pressedBy"))
            assertTrue(touch(MotionEvent.ACTION_DOWN, 1 to (x to edge - 6f)))
            assertEquals(1, field(left, "pressedBy"))
            touch(MotionEvent.ACTION_UP, 1 to (x to edge - 6f))
        }
    }

    private fun control(id: String): Any = (field(view, "controls") as List<*>).first { field(it!!, "id") == id }!!
    private fun editor() {
        view = OnScreenControls(context, null, editing = true)
        view.layout(0, 0, 1200, 800)
    }

    @Test fun keyboardShortcutWorksWithHiddenPadAndButtonsOnly() {
        var opened = 0
        view = OnScreenControls(context, null, onKeyboard = { opened++ })
        view.layout(0, 0, 1200, 800)
        view.setQuickHidden(true)
        assertNotEquals(0, pixel(90f, 766f))
        touch(MotionEvent.ACTION_DOWN, 1 to (90f to 766f))
        assertEquals(0, opened)
        touch(MotionEvent.ACTION_UP, 1 to (90f to 766f))
        assertEquals(1, opened)
        assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (600f to 400f)))
        view.setButtonsOnly(true)
        touch(MotionEvent.ACTION_DOWN, 1 to (600f to 756f))
        touch(MotionEvent.ACTION_UP, 1 to (600f to 756f))
        assertEquals(2, opened)
        assertEquals(-1, field(control("guide"), "pressedBy"))
    }

    @Test fun cancelledDraggedOrDisabledKeyboardShortcutDoesNotOpen() {
        var opened = 0
        view = OnScreenControls(context, null, onKeyboard = { opened++ })
        view.layout(0, 0, 1200, 800)
        touch(MotionEvent.ACTION_DOWN, 1 to (90f to 766f))
        touch(MotionEvent.ACTION_CANCEL, 1 to (90f to 766f))
        touch(MotionEvent.ACTION_UP, 1 to (90f to 766f))
        assertEquals(0, opened)
        touch(MotionEvent.ACTION_DOWN, 1 to (90f to 766f))
        touch(MotionEvent.ACTION_MOVE, 1 to (180f to 766f))
        touch(MotionEvent.ACTION_UP, 1 to (180f to 766f))
        assertEquals(0, opened)
        ControllerPrefs.setKeyboardButton(context, false)
        view.reload()
        view.setQuickHidden(true)
        assertEquals(0, pixel(90f, 766f))
        assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (90f to 766f)))
    }

    @Test fun hidingSystemButtonsRemovesHitTargetsInBothOverlayModes() {
        for (buttonsOnly in listOf(false, true)) {
            ControllerPrefs.setSteamButton(context, true)
            ControllerPrefs.setQamButton(context, true)
            view.reload()
            view.setButtonsOnly(buttonsOnly)
            val guide = control("guide")
            val qam = control("qam")
            val gx = value(guide, "cx"); val gy = value(guide, "cy")
            val qx = value(qam, "cx"); val qy = value(qam, "cy")
            assertNotEquals(0, pixel(gx, gy))
            assertNotEquals(0, pixel(qx, qy))
            touch(MotionEvent.ACTION_DOWN, 1 to (gx to gy))
            ControllerPrefs.setSteamButton(context, false)
            view.reload()
            assertEquals(-1, field(guide, "pressedBy"))
            assertEquals(0, pixel(gx, gy))
            assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (gx to gy)))
            assertTrue(touch(MotionEvent.ACTION_DOWN, 1 to (qx to qy)))
            touch(MotionEvent.ACTION_UP, 1 to (qx to qy))
            ControllerPrefs.setQamButton(context, false)
            view.reload()
            assertEquals(0, pixel(qx, qy))
            assertFalse(touch(MotionEvent.ACTION_DOWN, 1 to (qx to qy)))
        }
    }
    private fun field(owner: Any, name: String): Any = owner.javaClass.getDeclaredField(name).apply { isAccessible = true }.get(owner)!!
    private fun nullableFloat(owner: Any, name: String) = owner.javaClass.getDeclaredField(name).apply { isAccessible = true }.get(owner) as Float?
    private fun value(owner: Any, name: String) = field(owner, name) as Float

    private fun pixel(x: Float, y: Float): Int {
        val bitmap = Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888)
        view.draw(Canvas(bitmap))
        val color = bitmap.getPixel(x.toInt(), y.toInt())
        bitmap.recycle()
        return color
    }

    private fun touch(action: Int, vararg pointers: Pair<Int, Pair<Float, Float>>): Boolean {
        val event = event(action, *pointers)
        return try { view.onTouchEvent(event) } finally { event.recycle() }
    }

    private fun dispatch(root: View, action: Int, vararg pointers: Pair<Int, Pair<Float, Float>>): Boolean {
        val event = event(action, *pointers)
        return try { root.dispatchTouchEvent(event) } finally { event.recycle() }
    }

    private fun event(action: Int, vararg pointers: Pair<Int, Pair<Float, Float>>): MotionEvent {
        val properties = pointers.map { (id, _) -> MotionEvent.PointerProperties().apply { this.id = id; toolType = MotionEvent.TOOL_TYPE_FINGER } }.toTypedArray()
        val coords = pointers.map { (_, position) -> MotionEvent.PointerCoords().apply { x = position.first; y = position.second; pressure = 1f; size = 1f } }.toTypedArray()
        return MotionEvent.obtain(1000L, time++, action, pointers.size, properties, coords, 0, 0, 1f, 1f, 0, 0, 0, 0)
    }

    // Keep real PadBridge/PadState mutations, but leave the Android JNI ring transport closed.
    @Implements(value = FakeInputWriter::class, isInAndroidSdk = false)
    class ClosedInputWriter {
        @Implementation fun open(): Boolean = false

        companion object {
            @JvmStatic @Implementation fun __staticInitializer__() = Unit
        }
    }
}
