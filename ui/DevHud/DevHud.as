package
{
	import f4mp.ui.Marker;
	import f4mp.ui.Style;

	import flash.display.MovieClip;
	import flash.display.Shape;
	import flash.display.Sprite;
	import flash.text.TextField;
	import flash.text.TextFormat;

	// The developer HUD's movie (src/game/UiDevHud.cpp loads it as Interface/F4Multiplayer/DevHud.swf): lines of text in a
	// monospace font at the top right of the screen, on a dark ground so they read over any scene, with each player's
	// compass marker beside their row. It takes no input. The plugin gives it the text a few times a second (SetText), from
	// inside the menu's own update.
	public class DevHud extends MovieClip
	{
		// The game's font library has Consolas as this in every language (Interface\FontConfig*.txt); the terminals' font
		// is another font in Chinese and Japanese, and the columns must line up.
		private static const FONT:String = "$DebugTextFont";
		private static const RIGHT:Number = 1280;  // the very corner of the game's 1280x720 stage
		private static const TOP:Number = 0;
		private static const MARGIN:Number = 4;    // the ground past the text, at the right
		private static const GUTTER:Number = 12;   // left of the text, for the markers (the ground's left edge is its own)
		private static const MIN_WIDTH:Number = 360;  // the text's room at least: the usual lines fit, so the box stays put
		private static const SIZE:Number = 10;
		private static const MARKER_SIZE:Number = 0.55;  // of the compass's marker
		private static const SHADOW_SCALE:Number = 0.5;
		private static const WIDTH_STEP:Number = 40;  // past MIN_WIDTH it grows by steps, so it doesn't twitch as numbers change
		private static const LATE:uint = 0xFFB000;     // an age past a second
		private static const STALE:uint = 0xFF4040;    // past five

		private var ground:Shape = new Shape();
		private var markers:Sprite = new Sprite();
		private var text:TextField;

		public function DevHud()
		{
			super();
			mouseEnabled = false;
			mouseChildren = false;
			text = Style.Text("", SIZE, 0xFFFFFF, SHADOW_SCALE, FONT);
			text.multiline = true;
			text.wordWrap = false;
			text.y = TOP;
			addChild(ground);
			addChild(text);
			addChild(markers);
		}

		// a_color: the player's HUD color (a menu of ours isn't tinted by the game's renderer, so the movie is told it).
		// a_rows: "line,color,shape,ageFrom,ageTo,ageLevel;" for each player's row (UiDevHud.cpp).
		public function SetText(a_text:String, a_color:uint, a_rows:String):void
		{
			var format:TextFormat = text.defaultTextFormat;
			format.color = a_color;
			text.defaultTextFormat = format;
			text.text = a_text;
			// The text's left edge stays where it was while the lines fit: only a longer one moves it, a step at a time.
			var room:Number = Math.max(MIN_WIDTH, Math.ceil(text.width / WIDTH_STEP) * WIDTH_STEP);
			text.x = RIGHT - MARGIN - room;
			var left:Number = text.x - GUTTER;
			ground.graphics.clear();
			ground.graphics.beginFill(0x000000, 0.45);
			ground.graphics.drawRect(left, TOP, RIGHT - left, text.height);
			ground.graphics.endFill();

			Style.Clear(markers);
			var lines:int = Math.max(1, text.numLines);
			var lineHeight:Number = (text.height - 2 * Style.TEXT_GUTTER) / lines;
			var rows:Array = a_rows.split(";");
			for (var i:int = 0; i < rows.length; ++i) {
				var parts:Array = String(rows[i]).split(",");
				if (parts.length < 6) {
					continue;
				}
				var line:int = int(parts[0]);
				var marker:Shape = Marker.Draw(uint(parts[1]), int(parts[2]), MARKER_SIZE);
				marker.x = text.x - GUTTER / 2;
				marker.y = text.y + Style.TEXT_GUTTER + lineHeight * (line + 0.5);
				markers.addChild(marker);
				var level:int = int(parts[5]);
				var from:int = int(parts[3]);
				var to:int = int(parts[4]);
				if (level > 0 && to > from && to <= text.length) {
					var age:TextFormat = new TextFormat();
					age.color = level > 1 ? STALE : LATE;
					text.setTextFormat(age, from, to);
				}
			}
		}
	}
}
