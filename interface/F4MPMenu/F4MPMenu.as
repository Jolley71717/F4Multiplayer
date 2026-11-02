package
{
	import flash.display.MovieClip;
	import flash.display.Shape;
	import flash.display.Sprite;
	import flash.events.MouseEvent;
	import flash.filters.DropShadowFilter;
	import flash.text.TextField;
	import flash.text.TextFieldAutoSize;
	import flash.text.TextFormat;

	// The multiplayer menu's movie (src/game/MpMenu.cpp loads it as Interface/F4MPMenu.swf): a second column beside the
	// main menu's list, laid out as the game's own (Settings, Add-ons) are: just right of the main panel, its bottom
	// level with it, brackets along its top and bottom, the chosen entry a bar across it, a small footer at its bottom
	// right, and the back hint at the bottom of the screen. Everything is in the player's HUD color and scaled to the main
	// panel's height. The plugin fills it (SetEntries) and is told what the player does through BGSCodeObj. Keys and the
	// gamepad come as the game's user events (ProcessUserEvent); the mouse is handled here. Only what Flash Player 11.2 had
	// is used: Scaleform GFx 4 has no more.
	public class F4MPMenu extends MovieClip
	{
		// Set by the plugin (MpMenu SetCode): Pressed(index), Close(), PlaySound(name), Log(text).
		public var BGSCodeObj:Object;

		private static const FONT:String = "$MAIN_Font";  // the game's font library maps it to the language's font
		private static const STAGE_W:Number = 1280;
		private static const STAGE_H:Number = 720;

		// Measured from the game's own second column against its main panel, in units of the main panel's height / 652.
		private static const REF_HEIGHT:Number = 652;
		private static const GAP:Number = 10;           // main panel to ours
		private static const TOP_DROP:Number = 40;      // our top below the main panel's
		private static const FIRST_ROW:Number = 69;     // our top to the first bar
		private static const ROW_PITCH:Number = 58.5;
		private static const BAR_HEIGHT:Number = 52;
		private static const BAR_INSET:Number = 27;     // the bar's left and right ends from the panel's sides
		private static const TEXT_INSET:Number = 52;    // the text's left from the panel's side
		private static const TEXT_SIZE:Number = 38;
		private static const MIN_WIDTH:Number = 520;
		private static const TICK:Number = 14;          // the brackets' ends
		private static const FOOTER_SIZE:Number = 22;
		private static const FOOTER_INSET:Number = 18;
		private static const HINT_SIZE:Number = 34;
		private static const HINT_FROM_BOTTOM:Number = 92;

		private static const BLACK:uint = 0x000000;

		private var panel:Sprite;
		private var hint:Sprite;
		private var rows:Array = [];
		private var selected:int = -1;
		private var color:uint = 0x12FF15;  // the player's HUD color, from the plugin
		private var scale:Number = 0.5;
		private var panelWidth:Number = 0;

		public function F4MPMenu()
		{
			super();
			panel = new Sprite();
			hint = new Sprite();
			addChild(panel);
			addChild(hint);
		}

		// From the plugin: the entries' labels; the main panel's box on the stage (this movie's stage has the same size and
		// scale mode as the main menu's); the HUD color; the footer text and the back hint's word.
		public function SetEntries(a_labels:Array, a_left:Number, a_top:Number, a_right:Number, a_bottom:Number, a_color:uint, a_footer:String,
			a_back:String):void
		{
			color = a_color;
			scale = (a_bottom - a_top) / REF_HEIGHT;
			while (panel.numChildren > 0) {
				panel.removeChildAt(0);
			}
			rows = [];

			var labels:Array = [];
			var widest:Number = 0;
			for (var i:int = 0; i < a_labels.length; ++i) {
				var label:TextField = Text(String(a_labels[i]), TEXT_SIZE * scale, color);
				labels.push(label);
				widest = Math.max(widest, label.width);
			}
			panelWidth = Math.max(MIN_WIDTH * scale, widest + (TEXT_INSET + BAR_INSET) * 2 * scale);
			var top:Number = a_top + TOP_DROP * scale;
			var height:Number = a_bottom - top;
			panel.x = a_right + GAP * scale;
			panel.y = top;

			panel.addChild(Background(panelWidth, height));
			panel.addChild(Brackets(panelWidth, height));
			for (i = 0; i < labels.length; ++i) {
				var row:Sprite = Row(labels[i], i);
				row.y = (FIRST_ROW + ROW_PITCH * i) * scale;
				panel.addChild(row);
				rows.push(row);
			}
			var footer:TextField = Text(a_footer, FOOTER_SIZE * scale, color);
			footer.x = panelWidth - footer.width - FOOTER_INSET * scale;
			footer.y = height - footer.height - FOOTER_INSET * scale * 0.6;
			panel.addChild(footer);

			Hint(a_back);
			selected = -1;
			Select(rows.length > 0 ? 0 : -1, false);
		}

		// The game's user events, keyboard and gamepad alike. True: taken here.
		public function ProcessUserEvent(a_event:String, a_pressed:Boolean):Boolean
		{
			if (!a_pressed) {
				return false;
			}
			switch (a_event) {
				case "Up":
					Select(selected <= 0 ? rows.length - 1 : selected - 1, true);
					return true;
				case "Down":
					Select(selected >= rows.length - 1 ? 0 : selected + 1, true);
					return true;
				case "Accept":
					Press(selected);
					return true;
				case "Cancel":
				case "Back":
					Back();
					return true;
			}
			return false;
		}

		// A dark green ground: darker than the scene, washed with the HUD color, as the game's panels are.
		private function Background(a_width:Number, a_height:Number):Shape
		{
			var shape:Shape = new Shape();
			shape.graphics.beginFill(BLACK, 0.45);
			shape.graphics.drawRect(0, 0, a_width, a_height);
			shape.graphics.endFill();
			shape.graphics.beginFill(color, 0.16);
			shape.graphics.drawRect(0, 0, a_width, a_height);
			shape.graphics.endFill();
			return shape;
		}

		// Along the top and the bottom, each with its ends turned in, drawn inside the box, with a hard black shadow.
		private function Brackets(a_width:Number, a_height:Number):Shape
		{
			var stroke:Number = Math.max(1, 2 * scale);
			var half:Number = stroke / 2;
			var tick:Number = TICK * scale;
			var shape:Shape = new Shape();
			shape.graphics.lineStyle(stroke, color, 1, false, "normal", "none", "miter");
			shape.graphics.moveTo(half, half + tick);
			shape.graphics.lineTo(half, half);
			shape.graphics.lineTo(a_width - half, half);
			shape.graphics.lineTo(a_width - half, half + tick);
			shape.graphics.moveTo(half, a_height - half - tick);
			shape.graphics.lineTo(half, a_height - half);
			shape.graphics.lineTo(a_width - half, a_height - half);
			shape.graphics.lineTo(a_width - half, a_height - half - tick);
			shape.filters = [ Shadow() ];
			return shape;
		}

		private function Shadow():DropShadowFilter
		{
			return new DropShadowFilter(Math.max(1, 2 * scale), 45, BLACK, 0.85, 1, 1, 1, 1);
		}

		private function Text(a_text:String, a_size:Number, a_color:uint):TextField
		{
			var text:TextField = new TextField();
			text.embedFonts = true;
			text.selectable = false;
			text.mouseEnabled = false;
			text.autoSize = TextFieldAutoSize.LEFT;
			text.defaultTextFormat = new TextFormat(FONT, a_size, a_color);
			text.text = a_text;
			text.filters = [ Shadow() ];
			return text;
		}

		private function Row(a_text:TextField, a_index:int):Sprite
		{
			var row:Sprite = new Sprite();
			var bar:Shape = new Shape();
			bar.name = "bar";
			bar.graphics.beginFill(color, 1);
			bar.graphics.drawRect(BAR_INSET * scale, 0, panelWidth - BAR_INSET * 2 * scale, BAR_HEIGHT * scale);
			bar.graphics.endFill();
			bar.visible = false;
			row.addChild(bar);

			a_text.name = "text";
			a_text.x = TEXT_INSET * scale;
			a_text.y = (BAR_HEIGHT * scale - a_text.height) / 2;
			row.addChild(a_text);

			row.buttonMode = true;
			row.addEventListener(MouseEvent.ROLL_OVER, function(a_e:MouseEvent):void { Select(a_index, true); });
			row.addEventListener(MouseEvent.CLICK, function(a_e:MouseEvent):void { Press(a_index); });
			return row;
		}

		// "[Esc) BACK]" at the bottom of the screen, as the game's menus show it; a click goes back too.
		private function Hint(a_back:String):void
		{
			while (hint.numChildren > 0) {
				hint.removeChildAt(0);
			}
			var text:TextField = Text("Esc) " + a_back, HINT_SIZE * scale, color);
			var pad:Number = 12 * scale;
			var w:Number = text.width + pad * 2;
			var h:Number = text.height;
			text.x = pad;
			hint.addChild(text);

			var stroke:Number = Math.max(1, 2 * scale);
			var tick:Number = 8 * scale;
			var frame:Shape = new Shape();
			frame.graphics.beginFill(BLACK, 0.001);  // the whole hint takes the click, not just its glyphs
			frame.graphics.drawRect(0, 0, w, h);
			frame.graphics.endFill();
			frame.graphics.lineStyle(stroke, color, 1, false, "normal", "none", "miter");
			frame.graphics.moveTo(tick, 0);
			frame.graphics.lineTo(0, 0);
			frame.graphics.lineTo(0, h);
			frame.graphics.lineTo(tick, h);
			frame.graphics.moveTo(w - tick, 0);
			frame.graphics.lineTo(w, 0);
			frame.graphics.lineTo(w, h);
			frame.graphics.lineTo(w - tick, h);
			frame.filters = [ Shadow() ];
			hint.addChildAt(frame, 0);

			hint.x = (STAGE_W - w) / 2;
			hint.y = STAGE_H - HINT_FROM_BOTTOM * scale - h;
			hint.buttonMode = true;
			if (!hint.hasEventListener(MouseEvent.CLICK)) {
				hint.addEventListener(MouseEvent.CLICK, function(a_e:MouseEvent):void { Back(); });
			}
		}

		private function Select(a_index:int, a_sound:Boolean):void
		{
			if (a_index == selected) {
				return;
			}
			if (selected >= 0 && selected < rows.length) {
				Paint(rows[selected], false);
			}
			selected = a_index;
			if (selected >= 0 && selected < rows.length) {
				Paint(rows[selected], true);
				if (a_sound) {
					Code("PlaySound", "UIMenuFocus");
				}
			}
		}

		// The chosen entry: the bar in the HUD color, its text black with no shadow.
		private function Paint(a_row:Sprite, a_on:Boolean):void
		{
			a_row.getChildByName("bar").visible = a_on;
			var text:TextField = a_row.getChildByName("text") as TextField;
			text.textColor = a_on ? BLACK : color;
			text.filters = a_on ? [] : [ Shadow() ];
		}

		private function Press(a_index:int):void
		{
			if (a_index < 0 || a_index >= rows.length) {
				return;
			}
			Code("PlaySound", "UIMenuOK");
			Code("Pressed", a_index);
		}

		private function Back():void
		{
			Code("PlaySound", "UIMenuCancel");
			Code("Close");
		}

		// A call into the plugin. With no code object (the movie opened outside the game) nothing happens.
		private function Code(a_name:String, a_arg:* = undefined):void
		{
			if (BGSCodeObj == null) {
				return;
			}
			try {
				if (a_arg === undefined) {
					BGSCodeObj[a_name]();
				} else {
					BGSCodeObj[a_name](a_arg);
				}
			} catch (e:Error) {
			}
		}
	}
}
