package
{
	import f4mp.ui.Style;

	import flash.display.MovieClip;
	import flash.display.Shape;
	import flash.text.TextField;
	import flash.text.TextFormat;
	import flash.text.TextFormatAlign;

	// The developer HUD's movie (src/game/UiDevHud.cpp loads it as Interface/F4Multiplayer/DevHud.swf): lines of text at the
	// top right of the screen, on a dark ground so they read over any scene. It takes no input. The plugin gives it the
	// text a few times a second (SetText), from inside the menu's own update.
	public class DevHud extends MovieClip
	{
		private static const RIGHT:Number = 1280;  // the very corner of the game's 1280x720 stage
		private static const TOP:Number = 0;
		private static const WIDTH:Number = 460;
		private static const SIZE:Number = 10;
		private static const SHADOW_SCALE:Number = 0.5;
		private static const GROUND_STEP:Number = 20;  // the ground's width goes by steps, so it doesn't twitch as numbers change

		private var ground:Shape = new Shape();
		private var text:TextField;

		public function DevHud()
		{
			super();
			mouseEnabled = false;
			mouseChildren = false;
			text = Style.Wrapped("", WIDTH, SIZE, 0xFFFFFF, SHADOW_SCALE);
			text.x = RIGHT - WIDTH;
			text.y = TOP;
			addChild(ground);
			addChild(text);
		}

		// a_color: the player's HUD color. A menu of ours isn't tinted by the game's renderer, so the movie is told it.
		public function SetText(a_text:String, a_color:uint):void
		{
			var format:TextFormat = text.defaultTextFormat;
			format.align = TextFormatAlign.RIGHT;
			format.color = a_color;
			text.defaultTextFormat = format;
			text.text = a_text;
			var width:Number = Math.ceil(text.textWidth / GROUND_STEP) * GROUND_STEP + 4;
			ground.graphics.clear();
			ground.graphics.beginFill(0x000000, 0.45);
			ground.graphics.drawRect(RIGHT - width, TOP, width, text.height);
			ground.graphics.endFill();
		}
	}
}
