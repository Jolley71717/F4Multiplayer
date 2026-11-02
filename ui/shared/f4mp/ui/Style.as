package f4mp.ui
{
	import flash.display.Sprite;
	import flash.filters.DropShadowFilter;
	import flash.text.TextField;
	import flash.text.TextFieldAutoSize;
	import flash.text.TextFormat;

	// The game's menu look, shared by the plugin's movies: its fonts, its hard shadow, its text. Sizes are given already
	// scaled; a movie scales them to the game's panels it lines up with. Only what Flash Player 11.2 had is used:
	// Scaleform GFx 4 has no more.
	public final class Style
	{
		public static const FONT:String = "$MAIN_Font";  // the game's font library maps it to the language's font
		// The game's controller glyphs: a letter each in this font (Shared.AS3.BSButtonHint's NameToTextMap, Xenon_B = "B").
		public static const BUTTONS_FONT:String = "$Controller_buttons";
		public static const PAD_B:String = "B";
		public static const BLACK:uint = 0x000000;
		public static const WHITE:uint = 0xFFFFFF;
		// The game's menu movies are 1280x720, shown whole; ours are made the same (tools/flash/build.sh).
		public static const STAGE_W:Number = 1280;
		public static const STAGE_H:Number = 720;

		// The game's hard shadow: down and right by the same amount, no blur, black.
		public static function Shadow(a_scale:Number):DropShadowFilter
		{
			return new DropShadowFilter(Math.max(1, Math.round(2 * a_scale)), 45, BLACK, 1, 0, 0, 1, 1);
		}

		public static function Text(a_text:String, a_size:Number, a_color:uint, a_scale:Number, a_font:String = FONT):TextField
		{
			var text:TextField = new TextField();
			text.embedFonts = true;
			text.selectable = false;
			text.mouseEnabled = false;
			text.autoSize = TextFieldAutoSize.LEFT;
			text.defaultTextFormat = new TextFormat(a_font, a_size, a_color);
			text.text = a_text;
			text.filters = [ Shadow(a_scale) ];
			return text;
		}

		public static function Clear(a_sprite:Sprite):void
		{
			while (a_sprite.numChildren > 0) {
				a_sprite.removeChildAt(0);
			}
		}
	}
}
