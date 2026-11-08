package f4mp.ui
{
	import flash.display.Sprite;
	import flash.events.MouseEvent;
	import flash.text.TextField;

	// The menu's command bar, as the game's menus show theirs (their ButtonHintBar_mc): each command's key and what it does, in
	// brackets, on a ground of its own. For now the one command every page has, the way back: "[Esc) BACK]", or with a gamepad the B
	// glyph in the game's controller font. A click does it too. A page could have more, side by side as the game's do: Show would
	// take a list of commands, each with its key, word and action, and the brackets would go round each. Nothing needs that yet.
	public class CommandBar extends Sprite
	{
		private static const TEXT_SIZE:Number = 34;     // in the panel's units (see the movie's scale)
		private static const FROM_BOTTOM:Number = 92;
		private static const PAD:Number = 12;
		private static const KEY_GAP:Number = 8;
		private static const TICK:Number = 8;

		private var onBack:Function;

		public function CommandBar(a_onBack:Function)
		{
			super();
			onBack = a_onBack;
			buttonMode = true;
			addEventListener(MouseEvent.CLICK, function(a_e:MouseEvent):void { onBack(); });
		}

		public function Show(a_word:String, a_key:String, a_gamepad:Boolean, a_color:uint, a_scale:Number):void
		{
			Style.Clear(this);
			var pad:Number = PAD * a_scale;
			var x:Number = pad;
			var h:Number = 0;
			if (a_gamepad) {
				var glyph:TextField = Style.Text(Style.PAD_B, TEXT_SIZE * a_scale, a_color, a_scale, Style.BUTTONS_FONT);
				glyph.x = x;
				addChild(glyph);
				x += glyph.width + 4 * a_scale;
				h = glyph.height;
			}
			if (!a_gamepad) {
				var key:TextField = Style.Text(a_key, TEXT_SIZE * a_scale, a_color, a_scale);
				key.x = x;
				addChild(key);
				x += key.width + KEY_GAP * a_scale;
				h = key.height;
			}
			// The word in the bold of the game's headings: it's what the command is.
			var text:TextField = Style.Text(a_word, TEXT_SIZE * a_scale, a_color, a_scale, Style.BOLD_FONT);
			text.x = x;
			addChild(text);
			var w:Number = x + text.width + pad;
			h = Math.max(h, text.height);
			text.y = (h - text.height) / 2;
			addChildAt(Brackets.Hint(w, h, TICK * a_scale, a_color, a_scale), 0);
			this.x = Math.round((Style.STAGE_W - w) / 2);
			this.y = Math.round(Style.STAGE_H - FROM_BOTTOM * a_scale - h);
		}
	}
}
