package
{
	import flash.display.MovieClip;
	import flash.display.Shape;
	import flash.display.Sprite;

	// The push-to-talk HUD's movie (src/game/UiTalkHud.cpp loads it as Interface/F4Multiplayer/TalkHud.swf): a microphone with two
	// brackets each side, between the crit meter and the fatigue head, shown while the player transmits. The brackets light
	// with the mic's level. It takes no input. The plugin sets everything through SetState, from inside the menu's own update.
	public class TalkHud extends MovieClip
	{
		private static const CENTER_X:Number = 907;  // on the game's 1280x720 stage
		private static const CENTER_Y:Number = 658;
		private static const SCALE:Number = 0.75;
		private static const STROKE:Number = 4;
		private static const SHADOW:Number = 2;  // the shadow's offset, down and right, as the game's hard shadow

		private var icon:Sprite = new Sprite();
		private var mic:Shape = new Shape();
		private var inner:Shape = new Shape();
		private var outer:Shape = new Shape();
		// The same symbol in black under it: its shadow, as bright as what it's under.
		private var micShadow:Shape = new Shape();
		private var innerShadow:Shape = new Shape();
		private var outerShadow:Shape = new Shape();
		private var color:int = -1;

		public function TalkHud()
		{
			super();
			mouseEnabled = false;
			mouseChildren = false;
			icon.x = CENTER_X;
			icon.y = CENTER_Y;
			icon.scaleX = SCALE;
			icon.scaleY = SCALE;
			for each (var shadow:Shape in [ outerShadow, innerShadow, micShadow ]) {
				shadow.x = SHADOW;
				shadow.y = SHADOW;
				icon.addChild(shadow);
			}
			icon.addChild(outer);
			icon.addChild(inner);
			icon.addChild(mic);
			icon.visible = false;
			addChild(icon);
		}

		public function SetState(a_on:Boolean, a_inner:Number, a_outer:Number, a_color:uint):void
		{
			if (color != int(a_color)) {
				color = int(a_color);
				Draw(mic, inner, outer, a_color);
				Draw(micShadow, innerShadow, outerShadow, 0x000000);
			}
			icon.visible = a_on;
			inner.alpha = a_inner;
			outer.alpha = a_outer;
			innerShadow.alpha = a_inner;
			outerShadow.alpha = a_outer;
		}

		private function Draw(a_mic:Shape, a_inner:Shape, a_outer:Shape, a_color:uint):void
		{
			a_mic.graphics.clear();
			a_mic.graphics.beginFill(a_color);
			a_mic.graphics.drawRoundRect(-6.5, -22, 13, 27, 13, 13);
			a_mic.graphics.endFill();
			a_mic.graphics.lineStyle(STROKE, a_color, 1, false, "normal", "round");
			a_mic.graphics.moveTo(-14, -3);
			a_mic.graphics.lineTo(-14, 1);
			a_mic.graphics.curveTo(-14, 15, 0, 15);
			a_mic.graphics.curveTo(14, 15, 14, 1);
			a_mic.graphics.lineTo(14, -3);
			a_mic.graphics.moveTo(0, 15);
			a_mic.graphics.lineTo(0, 24);
			a_mic.graphics.moveTo(-8, 25);
			a_mic.graphics.lineTo(8, 25);
			Bracket(a_inner, a_color, 21, 27, 12);
			Bracket(a_outer, a_color, 30, 39, 19);
		}

		// One bracket each side, bowing out from a_near at its ends to a_far at its middle.
		private function Bracket(a_shape:Shape, a_color:uint, a_near:Number, a_far:Number, a_half:Number):void
		{
			a_shape.graphics.clear();
			a_shape.graphics.lineStyle(STROKE, a_color, 1, false, "normal", "round");
			for each (var side:Number in [ -1, 1 ]) {
				a_shape.graphics.moveTo(side * a_near, -a_half);
				a_shape.graphics.curveTo(side * a_far, 0, side * a_near, a_half);
			}
		}
	}
}
