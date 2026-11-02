package f4mp.ui
{
	import flash.display.Sprite;
	import flash.events.MouseEvent;
	import flash.text.TextField;

	// The way back out, at the bottom of the screen as the game's menus show it: "[Esc) BACK]", or with a gamepad the B
	// glyph in the game's controller font. A click goes back too.
	public class BackHint extends Sprite
	{
		private static const TEXT_SIZE:Number = 34;     // in the panel's units (see the movie's scale)
		private static const FROM_BOTTOM:Number = 92;
		private static const PAD:Number = 12;
		private static const TICK:Number = 8;

		private var onBack:Function;

		public function BackHint(a_onBack:Function)
		{
			super();
			onBack = a_onBack;
			buttonMode = true;
			addEventListener(MouseEvent.CLICK, function(a_e:MouseEvent):void { onBack(); });
		}

		public function Show(a_word:String, a_gamepad:Boolean, a_color:uint, a_scale:Number):void
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
			var text:TextField = Style.Text((a_gamepad ? "" : "Esc) ") + a_word, TEXT_SIZE * a_scale, a_color, a_scale);
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
