package
{
	import f4mp.ui.BackHint;
	import f4mp.ui.Brackets;
	import f4mp.ui.EntryList;
	import f4mp.ui.ScrollBar;
	import f4mp.ui.Style;

	import flash.display.MovieClip;
	import flash.display.Shape;
	import flash.display.Sprite;
	import flash.events.Event;
	import flash.events.MouseEvent;
	import flash.text.TextField;
	import flash.text.TextFieldAutoSize;
	import flash.utils.getTimer;

	// The multiplayer menu's movie (src/game/MPMenu.cpp loads it as Interface/F4Multiplayer/MPMenu.swf): a second column
	// beside the main menu's list, laid out as the game's own (Settings, Add-ons) are: just right of the main panel, its
	// bottom level with it, scaled to its height, brackets along its top and bottom, the chosen entry a bar across it, the
	// mod's name and version at its bottom right, and the back hint at the bottom of the screen, all in the player's HUD
	// color. The plugin sets its place and look once (SetLook), gives it each page (SetPage) and is told what the player does
	// through BGSCodeObj. Keys and the gamepad come as the game's user events (ProcessUserEvent); the mouse is the list's and
	// the hint's own.
	public class MPMenu extends MovieClip
	{
		// Set by the plugin (MPMenu SetCode): Pressed(serial, row), Close() (the way back), PlaySound(name), Log(text).
		public var BGSCodeObj:Object;

		// Measured from the game's own second column against its main panel, in units of the main panel's height / 652.
		private static const REF_HEIGHT:Number = 652;
		private static const GAP:Number = 10;           // main panel to ours
		private static const TOP_DROP:Number = 40;      // our top below the main panel's
		private static const FIRST_ROW:Number = 69;     // our top to the first bar
		private static const ROW_PITCH:Number = 58.5;
		private static const BAR_HEIGHT:Number = 52;
		private static const BAR_INSET:Number = 27;     // the bar's ends from the panel's sides
		private static const TEXT_INSET:Number = 52;    // the panel's side to where the game's text would start
		private static const TICK:Number = 14;          // the brackets' ends

		// Ours, in the same units, set by eye in the game.
		private static const ROW_TEXT_INSET:Number = 39;     // the text's left: half the bar's padding of the game's column
		private static const ALIGN_DROP:Number = 2.5;        // down, to sit level with the main panel
		private static const TEXT_SIZE:Number = 38;
		private static const BODY_WIDTH:Number = 900;        // the panel's width, unless a row needs more
		private static const BODY_SIZE:Number = 28;
		private static const BODY_SHARE:Number = 0.55;       // of the panel's height, with many rows; a longer text scrolls
		private static const BODY_ABOVE_FOOTER:Number = 16;  // a text and its frame stop this far over the version
		private static const STAGE_MARGIN:Number = 40;       // the stage's right side to the panel's widest
		private static const LIST_SHARE:Number = 2 / 3;      // of the panel's width, on a page with text for its rows
		private static const HINT_GAP:Number = 16;
		private static const FRAME_PAD:Number = 6;           // a scrolling text to its frame
		private static const FRAME_TICK:Number = 6;
		private static const FOOTER_SIZE:Number = 17;
		private static const FOOTER_INSET:Number = 18;
		private static const HEADING_SIZE:Number = 44;
		private static const HEADING_TOP:Number = 21;
		private static const HEADING_NOTE_SIZE:Number = 22;
		private static const HEADING_NOTE_GAP:Number = 10;
		private static const HEADING_NOTE_RISE:Number = 3;   // the version off the name's bottom, on its line
		private static const HEADING_BELOW:Number = 12;

		// The plugin's shader targets (MPMenu.cpp): Panel_mc.Background_mc, Panel_mc.Content_mc and Hint_mc.
		private var panel:Sprite;
		private var background:Sprite;
		private var content:Sprite;
		private var list:EntryList;
		private var hint:BackHint;

		private var color:uint = 0x12FF15;  // the player's HUD color, from the plugin
		private var scale:Number = 0.5;
		private var centered:Boolean = false;  // in the world, with no main menu to line up beside
		private var panelHeight:Number = 0;
		private var footerText:String = "";
		private var modName:String = "";
		private var modVersion:String = "";
		private var serial:Number = 0;
		private var hints:Array = [];
		private var hintField:TextField = null;
		private var bodyField:TextField = null;
		private var bodyBar:ScrollBar = null;
		private var gamepad:Boolean = false;
		private var backWord:String = "";

		private static const REPEAT_DELAY:int = 350;  // ms held before Up or Down repeats
		private static const REPEAT_EVERY:int = 80;   // ms between repeats
		private var held:int = 0;                     // -1 Up, 1 Down, 0 neither
		private var nextRepeat:int = 0;

		public function MPMenu()
		{
			super();
			panel = Layer("Panel_mc");
			background = Layer("Background_mc");
			content = Layer("Content_mc");
			list = new EntryList(Pressed, function():void {
				Code("PlaySound", "UIMenuFocus");
				ShowChosen();
			});
			hint = new BackHint(Back);
			hint.name = "Hint_mc";
			panel.addChild(background);
			panel.addChild(content);
			panel.addEventListener(MouseEvent.MOUSE_WHEEL, Wheel);
			addChild(panel);
			addChild(hint);
			addEventListener(Event.ENTER_FRAME, Repeat);
			// The main list beside us is another movie's, under this menu: the mouse is passed on to it.
			if (stage != null) {
				stage.addEventListener(MouseEvent.MOUSE_MOVE, function(a_e:MouseEvent):void { Outside(a_e, false); });
				stage.addEventListener(MouseEvent.CLICK, function(a_e:MouseEvent):void { Outside(a_e, true); });
			}
		}

		// From the plugin, once: the main panel's box on the stage (this movie's stage has the same size and
		// scale mode as the main menu's); the HUD color; the mod's name and version and the back hint's word.
		// a_centered: the box gives only the size, and the panel goes in the middle of the stage.
		public function SetLook(a_left:Number, a_top:Number, a_right:Number, a_bottom:Number, a_color:uint, a_name:String, a_version:String, a_back:String, a_centered:Boolean):void
		{
			color = a_color;
			backWord = a_back;
			scale = (a_bottom - a_top) / REF_HEIGHT;
			modName = a_name;
			modVersion = a_version;
			footerText = a_name + " " + a_version;
			var top:Number = Math.round(a_top + TOP_DROP * scale);
			panelHeight = Math.round(a_bottom) - top;
			centered = a_centered;
			panel.x = Math.round(a_right + GAP * scale);
			panel.y = centered ? Math.round((Style.STAGE_H - panelHeight) / 2) : top + ALIGN_DROP;
			hint.Show(backWord, gamepad, color, scale);
		}

		// From the plugin: a page, with its serial for the presses to carry: its text across the top, its rows under it, and
		// beside them what the chosen row does (a_hints, one a row, "": nothing to say). The way back isn't a row: it's the
		// back hint. a_keepRow: this page again, with the same row chosen. a_heading: the mod's name and version on top.
		public function SetPage(a_serial:Number, a_body:String, a_rows:Array, a_hints:Array, a_keepRow:Boolean, a_heading:Boolean):void
		{
			var keptRow:int = list.selectedIndex;
			var keptFirst:int = list.firstRow;
			serial = a_serial;
			hints = a_hints;
			hintField = null;
			bodyField = null;
			bodyBar = null;
			Style.Clear(background);
			Style.Clear(content);

			var anyHint:Boolean = false;
			for (var h:int = 0; h < hints.length; ++h) {
				anyHint = anyHint || String(hints[h]) != "";
			}
			var labels:Array = [];
			var values:Array = [];
			var widest:Number = 0;
			for (var i:int = 0; i < a_rows.length; ++i) {
				var parts:Array = String(a_rows[i]).split("\t");  // a row's name, and what it's set to if anything
				var label:TextField = Style.Text(String(parts[0]), TEXT_SIZE * scale, color, scale);
				var value:TextField = parts.length > 1 ? Style.Text(String(parts[1]), TEXT_SIZE * scale, color, scale) : null;
				labels.push(label);
				values.push(value);
				widest = Math.max(widest, label.width + (value != null ? value.width + ROW_PITCH * scale : 0));
			}
			// The same width on every page, unless a row needs more.
			var rowsWidth:Number = widest + (TEXT_INSET + BAR_INSET) * 2 * scale;
			var width:Number = Math.max(BODY_WIDTH * scale, anyHint ? rowsWidth / LIST_SHARE : rowsWidth);
			width = Math.round(Math.min(width, centered ? Style.STAGE_W - STAGE_MARGIN * scale * 2 : Style.STAGE_W - panel.x - STAGE_MARGIN * scale));
			if (centered) {
				panel.x = Math.round((Style.STAGE_W - width) / 2);
			}
			var height:Number = panelHeight;
			var listWidth:Number = anyHint ? Math.round(width * LIST_SHARE) : width;

			background.addChild(Ground(width, height));
			content.addChild(Brackets.Panel(width, height, TICK * scale, color, scale));

			var footer:TextField = Style.Text(footerText, FOOTER_SIZE * scale, color, scale);
			footer.x = width - footer.width - FOOTER_INSET * scale;
			footer.y = height - footer.height - FOOTER_INSET * scale * 0.6;

			// The rows start at the same place on every page: a line is kept for the page's status, whether it has one or not.
			var bodyTop:Number = FIRST_ROW * scale * 0.25;
			var oneLine:TextField = Style.Text("Ag", BODY_SIZE * scale, color, scale);
			var listTop:Number = ListBelow(bodyTop + oneLine.height);
			if (a_heading) {
				var title:TextField = Style.Text(modName, HEADING_SIZE * scale, color, scale);
				title.x = ROW_TEXT_INSET * scale;
				title.y = HEADING_TOP * scale;
				content.addChild(title);
				var note:TextField = Style.Text(modVersion, HEADING_NOTE_SIZE * scale, color, scale);
				note.x = title.x + title.width + HEADING_NOTE_GAP * scale;
				note.y = title.y + title.height - note.height - HEADING_NOTE_RISE * scale;
				content.addChild(note);
				bodyTop = title.y + title.height + HEADING_BELOW * scale;
				listTop = ListBelow(bodyTop + oneLine.height);
			}
			if (a_body != "") {
				var body:TextField = Style.Wrapped(a_body, width - (ROW_TEXT_INSET + TEXT_INSET) * scale, BODY_SIZE * scale, color, scale);
				body.x = ROW_TEXT_INSET * scale;
				body.y = bodyTop;
				// With few rows the text has the room they leave, so a long text shows more of itself.
				var left:Number = footer.y - body.y - BODY_ABOVE_FOOTER * scale;
				var room:Number = a_rows.length > 0 ? Math.max(height * BODY_SHARE, left - (a_rows.length + 0.5) * ROW_PITCH * scale) : left;
				if (body.height > room) {
					body.autoSize = TextFieldAutoSize.NONE;
					body.height = room;
				}
				bodyField = body;
				content.addChild(body);
				if (body.maxScrollV > 1) {
					var pad:Number = FRAME_PAD * scale;
					var frameWidth:Number = Math.round(width - TEXT_INSET * scale);
					var frameHeight:Number = Math.round(body.height + pad * 2);
					var frame:Shape = Brackets.Scrolling(frameWidth, frameHeight, FRAME_TICK * scale, color, scale);
					frame.x = Math.round(TEXT_INSET * scale * 0.5);
					frame.y = Math.round(body.y - pad);
					content.addChild(frame);
					bodyBar = new ScrollBar(BodyTo, frameHeight - Brackets.THICK * 2, color, scale);
					bodyBar.x = frame.x + frameWidth - Brackets.THICK;
					bodyBar.y = frame.y + Brackets.THICK;
					content.addChild(bodyBar);
					ShowBodyBar();
				}
				listTop = Math.max(listTop, ListBelow(body.y + body.height));
			}

			if (anyHint) {
				var hintX:Number = listWidth + HINT_GAP * scale;
				hintField = Style.Wrapped("", width - hintX - TEXT_INSET * scale * 0.5, BODY_SIZE * scale, color, scale);
				hintField.x = hintX;
				hintField.y = listTop;
				content.addChild(hintField);
			}

			list.Build(labels, values, { pitch: ROW_PITCH * scale, barHeight: BAR_HEIGHT * scale, barInset: BAR_INSET * scale, textInset: ROW_TEXT_INSET * scale,
				width: listWidth, height: Math.max(BAR_HEIGHT * scale, footer.y - listTop) }, color, scale);
			if (a_keepRow) {
				list.Restore(keptRow, keptFirst);
			}
			list.y = listTop;
			content.addChild(list);
			content.addChild(footer);
			ShowChosen();
		}

		// Where the rows start under what ends at a_y, leaving the chevrons their room.
		// The page's status line changed and nothing else, as often as it likes: the rows and the chosen one stay as they are.
		public function SetStatus(a_text:String):void
		{
			if (bodyField != null) {
				bodyField.text = a_text;
			}
		}

		private function ListBelow(a_y:Number):Number
		{
			return Math.round(a_y + ROW_PITCH * scale * EntryList.CHEVRON_SLOT);
		}

		// What the chosen row does.
		private function ShowChosen():void
		{
			if (hintField != null) {
				var at:int = list.selectedIndex;
				var text:String = at >= 0 && at < hints.length ? String(hints[at]) : "";
				hintField.text = text.charAt(0).toUpperCase() + text.substr(1);
			}
		}

		private function ScrollBody(a_by:int):void
		{
			if (bodyField != null) {
				bodyField.scrollV += a_by;
				ShowBodyBar();
			}
		}

		// From its scrollbar.
		private function BodyTo(a_at:Number):void
		{
			bodyField.scrollV = 1 + Math.round(a_at * (bodyField.maxScrollV - 1));
			ShowBodyBar();
		}

		private function ShowBodyBar():void
		{
			if (bodyBar != null) {
				var shown:int = bodyField.bottomScrollV - bodyField.scrollV + 1;
				bodyBar.Update(shown / Math.max(1, bodyField.numLines), (bodyField.scrollV - 1) / Math.max(1, bodyField.maxScrollV - 1));
			}
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
			if (a_event == "Up" || a_event == "Down") {
				held = a_pressed ? (a_event == "Up" ? -1 : 1) : 0;
				if (a_pressed) {
					nextRepeat = getTimer() + REPEAT_DELAY;
					Step(held);
				}
				return true;
			}
			if (!a_pressed) {
				return false;
			}
			switch (a_event) {
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

		// Up or Down held: the game gives us the press and the release, so the repeat is ours.
		private function Repeat(a_e:Event):void
		{
			if (held != 0 && getTimer() >= nextRepeat) {
				nextRepeat = getTimer() + REPEAT_EVERY;
				Step(held);
			}
		}

		private function Step(a_by:int):void
		{
			if (list.length > 0) {
				list.Move(a_by);
			} else {
				ScrollBody(a_by);
			}
		}

		// Over the text (when it scrolls) the text, else the rows.
		private function Wheel(a_e:MouseEvent):void
		{
			var by:int = a_e.delta > 0 ? -1 : 1;
			if (bodyField != null && bodyField.maxScrollV > 1 && (list.length == 0 || content.mouseY < list.y)) {
				ScrollBody(by);
			} else if (list.length > 0) {
				list.Move(by);
			}
		}

		// The panel's ground is the renderer's (the plugin's shader target on Background_mc): this only gives it its size,
		// nearly invisible, as the movie's own color over it would hide it.
		private function Ground(a_width:Number, a_height:Number):Shape
		{
			var shape:Shape = new Shape();
			shape.graphics.beginFill(Style.WHITE, 0.001);
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
			if (BGSCodeObj != null) {
				try {
					BGSCodeObj.Pressed(serial, a_index);
				} catch (e:Error) {
				}
			}
		}

		private function Outside(a_e:MouseEvent, a_click:Boolean):void
		{
			if (centered || BGSCodeObj == null || panel.hitTestPoint(a_e.stageX, a_e.stageY, false) || hint.hitTestPoint(a_e.stageX, a_e.stageY, true)) {
				return;
			}
			try {
				BGSCodeObj.MouseOutside(a_e.stageX, a_e.stageY, a_click);
			} catch (e:Error) {
			}
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
