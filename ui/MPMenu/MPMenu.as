package
{
	import f4mp.ui.BackHint;
	import f4mp.ui.Brackets;
	import f4mp.ui.EntryList;
	import f4mp.ui.Style;

	import flash.display.MovieClip;
	import flash.display.Shape;
	import flash.display.Sprite;
	import flash.text.TextField;

	// The multiplayer menu's movie (src/game/MPMenu.cpp loads it as Interface/F4Multiplayer/MPMenu.swf): a second column
	// beside the main menu's list, laid out as the game's own (Settings, Add-ons) are: just right of the main panel, its
	// bottom level with it, scaled to its height, brackets along its top and bottom, the chosen entry a bar across it, the
	// mod's name and version at its bottom right, and the back hint at the bottom of the screen, all in the player's HUD
	// color. The plugin fills it (SetEntries) and is told what the player does through BGSCodeObj. Keys and the gamepad
	// come as the game's user events (ProcessUserEvent); the mouse is the list's and the hint's own.
	public class MPMenu extends MovieClip
	{
		// Set by the plugin (MPMenu SetCode): Pressed(index), Close(), PlaySound(name), Log(text).
		public var BGSCodeObj:Object;

		// Measured from the game's own second column against its main panel, in units of the main panel's height / 652.
		private static const REF_HEIGHT:Number = 652;
		private static const GAP:Number = 10;           // main panel to ours
		private static const TOP_DROP:Number = 40;      // our top below the main panel's
		private static const FIRST_ROW:Number = 69;     // our top to the first bar
		private static const ROW_PITCH:Number = 58.5;
		private static const BAR_HEIGHT:Number = 52;
		private static const BAR_INSET:Number = 27;     // the bar's ends from the panel's sides
		private static const TEXT_INSET:Number = 52;    // the text's left from the panel's side
		private static const TEXT_SIZE:Number = 38;
		private static const MIN_WIDTH:Number = 520;
		private static const TICK:Number = 14;          // the brackets' ends
		private static const FOOTER_SIZE:Number = 22;
		private static const FOOTER_INSET:Number = 18;

		// The plugin's shader targets, when it draws through the game's renderer: Panel_mc.Content_mc and Hint_mc.
		private var panel:Sprite;
		private var background:Sprite;
		private var content:Sprite;
		private var list:EntryList;
		private var hint:BackHint;

		private var hudColor:uint = 0x12FF15;  // the player's HUD color, from the plugin
		private var color:uint = 0x12FF15;     // what the content is drawn in
		private var scale:Number = 0.5;
		private var gamepad:Boolean = false;
		private var backWord:String = "";

		public function MPMenu()
		{
			super();
			panel = Layer("Panel_mc");
			background = Layer("Background_mc");
			content = Layer("Content_mc");
			list = new EntryList(Pressed, function():void { Code("PlaySound", "UIMenuFocus"); });
			hint = new BackHint(Back);
			hint.name = "Hint_mc";
			panel.addChild(background);
			panel.addChild(content);
			addChild(panel);
			addChild(hint);
		}

		// From the plugin: the entries' labels; the main panel's box on the stage (this movie's stage has the same size and
		// scale mode as the main menu's); the HUD color; the footer text and the back hint's word; and whether the game's
		// renderer tints it (then the content is white).
		public function SetEntries(a_labels:Array, a_left:Number, a_top:Number, a_right:Number, a_bottom:Number, a_color:uint, a_footer:String,
			a_back:String, a_shaded:Boolean):void
		{
			hudColor = a_color;
			color = a_shaded ? Style.WHITE : a_color;
			backWord = a_back;
			scale = (a_bottom - a_top) / REF_HEIGHT;
			Style.Clear(background);
			Style.Clear(content);

			var labels:Array = [];
			var widest:Number = 0;
			for (var i:int = 0; i < a_labels.length; ++i) {
				var label:TextField = Style.Text(String(a_labels[i]), TEXT_SIZE * scale, color, scale);
				labels.push(label);
				widest = Math.max(widest, label.width);
			}
			var width:Number = Math.round(Math.max(MIN_WIDTH * scale, widest + (TEXT_INSET + BAR_INSET) * 2 * scale));
			var top:Number = Math.round(a_top + TOP_DROP * scale);
			var height:Number = Math.round(a_bottom) - top;
			panel.x = Math.round(a_right + GAP * scale);
			panel.y = top;

			background.addChild(Ground(width, height));
			content.addChild(Brackets.Panel(width, height, TICK * scale, color, scale));
			list.Build(labels, { pitch: ROW_PITCH * scale, barHeight: BAR_HEIGHT * scale, barInset: BAR_INSET * scale, textInset: TEXT_INSET * scale,
				width: width }, color, scale);
			list.y = FIRST_ROW * scale;
			content.addChild(list);

			var footer:TextField = Style.Text(a_footer, FOOTER_SIZE * scale, color, scale);
			footer.x = width - footer.width - FOOTER_INSET * scale;
			footer.y = height - footer.height - FOOTER_INSET * scale * 0.6;
			content.addChild(footer);

			hint.Show(backWord, gamepad, color, scale);
		}

		// From the game (IMenu::RefreshPlatform), and again when the player changes device: 0 is the keyboard and mouse.
		public function SetPlatform(a_platform:uint, a_psn:Boolean):void
		{
			gamepad = a_platform != 0;
			Code("Log", "platform " + a_platform + (a_psn ? " (PlayStation)" : ""));
			if (backWord != "") {
				hint.Show(backWord, gamepad, color, scale);
			}
		}

		// The game's user events, keyboard and gamepad alike. True: taken here.
		public function ProcessUserEvent(a_event:String, a_pressed:Boolean):Boolean
		{
			if (!a_pressed) {
				return false;
			}
			switch (a_event) {
				case "Up":
					list.Move(-1);
					return true;
				case "Down":
					list.Move(1);
					return true;
				case "Accept":
					list.PressSelected();
					return true;
				case "Cancel":
				case "Back":
					Back();
					return true;
			}
			return false;
		}

		// The panel's ground: dark, washed with the HUD color, about as the game's own (its shaded background is the
		// renderer's). Never tinted, so drawn in the real color.
		private function Ground(a_width:Number, a_height:Number):Shape
		{
			var shape:Shape = new Shape();
			shape.graphics.beginFill(Style.BLACK, 0.45);
			shape.graphics.drawRect(0, 0, a_width, a_height);
			shape.graphics.endFill();
			shape.graphics.beginFill(hudColor, 0.16);
			shape.graphics.drawRect(0, 0, a_width, a_height);
			shape.graphics.endFill();
			return shape;
		}

		private function Layer(a_name:String):Sprite
		{
			var layer:Sprite = new Sprite();
			layer.name = a_name;
			return layer;
		}

		private function Pressed(a_index:int):void
		{
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
