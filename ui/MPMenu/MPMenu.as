package
{
	import f4mp.ui.Brackets;
	import f4mp.ui.CommandBar;
	import f4mp.ui.Dialog;
	import f4mp.ui.EntryList;
	import f4mp.ui.Logo;
	import f4mp.ui.Markdown;
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

	// The multiplayer menu's movie (src/game/UiPanel.cpp loads it as Interface/F4Multiplayer/MPMenu.swf): a second column
	// beside the main menu's list, laid out as the game's own (Settings, Add-ons) are: just right of the main panel, its
	// bottom level with it, scaled to its height, brackets along its top and bottom, the chosen entry a bar across it, the
	// logo and version over it, and the command bar at the bottom of the screen, all in the player's HUD
	// color. The plugin sets its place and look once (SetLook), gives it each page (SetPage) and is told what the player does
	// through BGSCodeObj. Keys and the gamepad come as the game's user events (ProcessUserEvent); the mouse is the list's and
	// the command bar's own.
	public class MPMenu extends MovieClip
	{
		// Set by the plugin (MPMenu SetCode): Pressed(serial, row), Close() (the way back), PlaySound(name), Log(text).
		public var BGSCodeObj:Object;

		// Measured from the game's own second column against its main panel, in units of the main panel's height / 652.
		private static const REF_HEIGHT:Number = 652;
		private static const COMMAND_BAR_BELOW:Number = 16;  // the panel's bottom to the command bar, in the middle of the screen
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
		private static const ALIGN_DROP:Number = 0.5;        // beside the main menu: the whole panel down, set by eye to sit level with it
		private static const TEXT_SIZE:Number = 38;
		private static const BODY_WIDTH:Number = 900;        // the panel's width, unless a row needs more
		private static const BODY_SIZE:Number = 28;
		private static const BODY_SHARE:Number = 0.55;       // of the panel's height, with many rows; a longer text scrolls
		private static const STAGE_MARGIN:Number = 40;       // the stage's right side to the panel's widest
		private static const LIST_SHARE:Number = 2 / 3;      // of the panel's width, on a page with text for its rows
		private static const PLAYERS_SHARE:Number = 0.55;  // of the panel's width, for the rows beside a list of players: the same in every language, and room for the longest
		private static const DETAILS_RISE:Number = 8;   // a player's details up towards their name: a text field's margins are wide
		private static const MARKER_ROOM:Number = 40;   // after a player's name, for their marker
		private static const MARKER_SIZE:Number = 1.4;    // of the compass's marker
		private static const DETAILS_SIZE:Number = 22;
		private static const PLAYER_GAP:Number = 6;
		private static const PLAYERS_GAP:Number = 8;    // the rows to the list of players
		private static const PLAYERS_INSET:Number = 7; // the list's frame to its text, left and right
		private static const SCROLL_ROOM:Number = 24;   // taken from what scrolls, for its scrollbar, only when it has one
		private static const WHEEL_ROW:Number = 60;     // the wheel's step along the list
		private static const HINT_GAP:Number = 16;
		private static const FRAME_PAD:Number = 6;           // a scrolling text to its frame
		private static const FRAME_TICK:Number = 6;
		private static const BOTTOM_INSET:Number = 18;       // what's in the panel stops this far over its bottom
		private static const LOGO_HEIGHT:Number = 34;        // over the panel, outside it, as the game's panels have their titles
		private static const LOGO_ABOVE:Number = 8;
		private static const VERSION_SIZE:Number = 22;
		private static const VERSION_GAP:Number = 10;
		private static const VERSION_RISE:Number = 3;        // the version's text field up from the logo's bottom, to sit on its line

		// The plugin's shader targets (UiPanel.cpp): Panel_mc.Background_mc, Panel_mc.Content_mc, CommandBar_mc and CommandBarBackground_mc.
		private var panel:Sprite;
		private var background:Sprite;
		private var content:Sprite;
		private var list:EntryList;
		private var commandBar:CommandBar;
		private var commandBarGround:Sprite = new Sprite();

		private var color:uint = 0x12FF15;  // the player's HUD color, from the plugin
		private var scale:Number = 0.5;
		private var centered:Boolean = false;  // in the world, with no main menu to line up beside
		private var panelHeight:Number = 0;
		private var modVersion:String = "";
		private var serial:Number = 0;
		private var hints:Array = [];
		private var hintField:TextField = null;
		private var bodyField:TextField = null;
		private var dialog:Dialog = null;  // over the page, with the keys going to it
		private var shownWidth:Number = 0;
		private var bodyBar:ScrollBar = null;
		private var players:Sprite = null;
		private var playerRows:Array = [];
		private var chosenPlayer:int = -1;  // the player's row chosen, or -1: the choice is in the rows
		private var builtMouseX:Number = 0;
		private var builtMouseY:Number = 0;
		private var playersBar:ScrollBar = null;
		private var playersAt:Number = 0;
		private var playersX:Number = 0;
		private var playersTop:Number = 0;
		private var playersHeight:Number = 0;
		private var playersShown:Number = 0;
		private var gamepad:Boolean = false;
		private var backWord:String = "";
		private var backKey:String = "Esc)";  // the way back's key as the plugin names it in the player's language

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
				ChoosePlayer(-1);
				ShowChosen();
			});
			commandBar = new CommandBar(Back);
			commandBar.name = "CommandBar_mc";
			commandBarGround.name = "CommandBarBackground_mc";
			panel.addChild(background);
			panel.addChild(content);
			panel.addEventListener(MouseEvent.MOUSE_WHEEL, Wheel);
			addChild(panel);
			addChild(commandBarGround);
			addChild(commandBar);
			addEventListener(Event.ENTER_FRAME, Repeat);
			// The main list beside us is another movie's, under this menu: the mouse is passed on to it.
			if (stage != null) {
				stage.addEventListener(MouseEvent.MOUSE_MOVE, function(a_e:MouseEvent):void { Outside(a_e, false); });
				stage.addEventListener(MouseEvent.CLICK, function(a_e:MouseEvent):void { Outside(a_e, true); });
			}
		}

		// From the plugin, once: the main panel's box on the stage (this movie's stage has the same size and
		// scale mode as the main menu's); the HUD color; the mod's version and the way back's word and key.
		// a_centered: the box gives only the size, and the panel goes in the middle of the stage.
		public function SetLook(a_left:Number, a_top:Number, a_right:Number, a_bottom:Number, a_color:uint, a_version:String, a_back:String, a_centered:Boolean, a_backKey:String = "Esc)"):void
		{
			color = a_color;
			backWord = a_back;
			backKey = a_backKey;
			scale = (a_bottom - a_top) / REF_HEIGHT;
			modVersion = a_version;
			centered = a_centered;
			// Beside the main menu, its top line runs through the middle of our logo. In the middle of the screen, the box's size less the drop.
			var top:Number = Math.round(a_top + (centered ? TOP_DROP * scale : (LOGO_ABOVE + LOGO_HEIGHT / 2) * scale + ALIGN_DROP));
			panelHeight = Math.round(a_bottom) - top;
			panel.x = Math.round(a_right + GAP * scale);
			panel.y = centered ? Math.round((Style.STAGE_H - panelHeight) / 2) : top + ALIGN_DROP;
			ShowCommandBar();
		}

		// The command bar: at the bottom of the screen beside the main menu, just under the panel in the middle of the screen.
		private function ShowCommandBar():void
		{
			commandBar.Show(backWord, backKey, gamepad, color, scale);
			if (centered) {
				commandBar.y = Math.round(panel.y + panelHeight + COMMAND_BAR_BELOW * scale + commandBar.height);
			} else {
				commandBar.y = Math.round(commandBar.y - commandBar.height);  // clear of what the game shows at the screen's bottom
			}
			// The renderer's see-through ground, as the panel has (UiPanel.cpp).
			Style.Clear(commandBarGround);
			commandBarGround.addChild(Ground(commandBar.width, commandBar.height));
			commandBarGround.x = commandBar.x;
			commandBarGround.y = commandBar.y;
		}

		// From the plugin: a page, with its serial for the presses to carry: its text across the top, its rows under it, and
		// beside them what the chosen row does (a_hints, one a row, "": nothing to say). The way back isn't a row: it's on the
		// command bar. a_keepRow: this page again, with the same row chosen. a_framed: its text in a frame, as About's is. a_markdown: its text is markdown.
		public function SetPage(a_serial:Number, a_body:String, a_rows:Array, a_hints:Array, a_keepRow:Boolean, a_framed:Boolean, a_players:Array, a_header:String, a_markdown:Boolean):void
		{
			dialog = null;  // Style.Clear takes it
			var keptRow:int = list.selectedIndex;
			var keptPlayer:int = a_keepRow ? chosenPlayer : -1;
			chosenPlayer = -1;
			playerRows = [];
			var keptFirst:int = list.firstRow;
			serial = a_serial;
			hints = a_hints;
			hintField = null;
			bodyField = null;
			players = null;
			playersBar = null;
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
			shownWidth = width;
			var height:Number = panelHeight;
			var listWidth:Number = a_players.length > 0 ? Math.round(width * PLAYERS_SHARE) : anyHint ? Math.round(width * LIST_SHARE) : width;

			background.addChild(Ground(width, height));
			content.addChild(Brackets.Panel(width, height, TICK * scale, color, scale));

			DrawLogo();
			var bottom:Number = height - BOTTOM_INSET * scale;

			// The rows start at the same place on every page: a line is kept for the page's status, whether it has one or not.
			var oneLine:TextField = Style.Text("Ag", BODY_SIZE * scale, color, scale);
			var bodyTop:Number = FIRST_ROW * scale * 0.25;
			var listTop:Number = ListBelow(bodyTop + oneLine.height);
			if (a_body != "") {
				listTop = Math.max(listTop, ListBelow(DrawBody(a_body, width, height, a_rows.length, bodyTop, bottom, a_framed, a_markdown)));
			}

			// The player a page is about, drawn as in the list, and not to be chosen.
			if (a_header != "") {
				var header:Sprite = Players([ a_header ], width - (ROW_TEXT_INSET + TEXT_INSET) * scale, false);
				header.x = ROW_TEXT_INSET * scale;
				header.y = bodyTop;
				content.addChild(header);
				listTop = Math.max(listTop, ListBelow(bodyTop + header.height));
			}

			if (anyHint) {
				var hintX:Number = listWidth + HINT_GAP * scale;
				hintField = Style.Wrapped("", width - hintX - TEXT_INSET * scale * 0.5, BODY_SIZE * scale, color, scale);
				hintField.x = hintX;
				hintField.y = listTop;
				content.addChild(hintField);
			}

			if (a_players.length > 0) {
				DrawPlayers(a_players, width, listWidth, listTop, bottom);
			}

			list.Build(labels, values, { pitch: ROW_PITCH * scale, barHeight: BAR_HEIGHT * scale, barInset: BAR_INSET * scale, textInset: ROW_TEXT_INSET * scale,
				width: listWidth, height: Math.max(BAR_HEIGHT * scale, bottom - listTop) }, color, scale);
			if (a_keepRow) {
				list.Restore(keptRow, keptFirst);
			}
			list.y = listTop;
			content.addChild(list);
			builtMouseX = mouseX;
			builtMouseY = mouseY;
			if (keptPlayer >= 0 && playerRows.length > 0) {
				ChoosePlayer(Math.min(keptPlayer, playerRows.length - 1));
			}
			ShowChosen();
		}

		// The logo and the version after it, over the panel, outside it, the logo's left in line with the page's text.
		private function DrawLogo():void
		{
			var logo:Logo = new Logo(LOGO_HEIGHT * scale, color);
			logo.y = Math.round(-logo.height - LOGO_ABOVE * scale);
			content.addChild(logo);
			var version:TextField = Style.Text("v" + modVersion, VERSION_SIZE * scale, color, scale);
			logo.x = Math.round(ROW_TEXT_INSET * scale + Style.TEXT_GUTTER);
			version.x = Math.round(logo.x + logo.width + VERSION_GAP * scale);
			version.y = Math.round(logo.y + logo.height - version.height + VERSION_RISE * scale);
			content.addChild(version);
		}

		// The page's text. Returns where it ends.
		private function DrawBody(a_body:String, a_width:Number, a_height:Number, a_rows:int, a_top:Number, a_bottom:Number, a_framed:Boolean, a_markdown:Boolean):Number
		{
			// To the frame's right as far in as it starts on the left; less the scrollbar's room if it won't fit.
			var bodyWidth:Number = a_width - ROW_TEXT_INSET * 2 * scale;
			var body:TextField = BodyField(a_body, bodyWidth, a_markdown);
			// With few rows the text has the room they leave, so a long text shows more of itself.
			var left:Number = a_bottom - a_top;
			var room:Number = a_rows > 0 ? Math.max(a_height * BODY_SHARE, left - (a_rows + 0.5) * ROW_PITCH * scale) : left;
			if (body.height > room) {
				body = BodyField(a_body, bodyWidth - SCROLL_ROOM * scale, a_markdown);
				body.autoSize = TextFieldAutoSize.NONE;
				body.height = room;
			}
			body.x = ROW_TEXT_INSET * scale;
			body.y = a_top;
			bodyField = body;
			content.addChild(body);
			// About always shows where its text ends; a long text anywhere has the handle too.
			if (body.maxScrollV > 1 || a_framed) {
				var pad:Number = FRAME_PAD * scale;
				bodyBar = ScrollFrame(Math.round(TEXT_INSET * scale * 0.5), Math.round(body.y - pad), Math.round(a_width - TEXT_INSET * scale), Math.round(body.height + pad * 2), false, body.maxScrollV > 1, BodyTo);
				ShowBodyBar();
			}
			return body.y + body.height;
		}

		private function BodyField(a_body:String, a_width:Number, a_markdown:Boolean):TextField
		{
			var field:TextField = Style.Wrapped(a_markdown ? "" : a_body, a_width, BODY_SIZE * scale, color, scale);
			if (a_markdown) {
				new Markdown(a_body).ApplyTo(field, BODY_SIZE * scale);
			}
			return field;
		}

		// The list of players at the right of the rows: as long a list as there are players, scrolled as a long text is when it
		// doesn't fit. The page is drawn again as they move, so where it was scrolled to stays.
		private function DrawPlayers(a_players:Array, a_width:Number, a_listWidth:Number, a_top:Number, a_bottom:Number):void
		{
			var outer:Number = a_width - a_listWidth - PLAYERS_GAP * scale - TEXT_INSET * scale * 0.5;
			var inset:Number = PLAYERS_INSET * scale;
			var room:Number = a_bottom - a_top;
			players = Players(a_players, outer - inset * 2, true);
			// The whole width, less the scrollbar's room only if it won't fit.
			var scrolls:Boolean = players.height > room;
			if (scrolls) {
				playerRows = [];
				players = Players(a_players, outer - SCROLL_ROOM * scale - inset * 2, true);
			}
			playersX = a_listWidth + PLAYERS_GAP * scale;
			players.x = playersX + inset;
			playersTop = a_top;
			content.addChild(players);
			playersHeight = players.height;
			playersShown = room;
			// Its top and bottom always show where it ends, with the same frame and handle as a long text's when it scrolls.
			var pad:Number = FRAME_PAD * scale;
			playersBar = ScrollFrame(Math.round(playersX - pad), Math.round(a_top - pad), Math.round(outer + pad), Math.round(room + pad * 2), scrolls, scrolls, PlayersTo);
			if (scrolls) {
				var clip:Shape = new Shape();
				clip.graphics.beginFill(Style.WHITE);
				clip.graphics.drawRect(0, 0, outer, room);
				clip.graphics.endFill();
				clip.x = playersX;
				clip.y = a_top;
				content.addChild(clip);
				players.mask = clip;
			} else {
				playersAt = 0;
			}
			PlayersTo(playersAt);
		}

		// The frame round what can scroll, and its handle when it does: returned, or null.
		private function ScrollFrame(a_x:Number, a_y:Number, a_width:Number, a_height:Number, a_rightLine:Boolean, a_scrolls:Boolean, a_onDrag:Function):ScrollBar
		{
			var frame:Shape = Brackets.Scrolling(a_width, a_height, FRAME_TICK * scale, color, scale, a_rightLine);
			frame.x = a_x;
			frame.y = a_y;
			content.addChild(frame);
			if (!a_scrolls) {
				return null;
			}
			var bar:ScrollBar = new ScrollBar(a_onDrag, a_height - Brackets.THICK * 2, color, scale);
			bar.x = a_x + a_width - Brackets.THICK;
			bar.y = a_y + Brackets.THICK;
			content.addChild(bar);
			return bar;
		}

		// From the plugin: a dialog over the page showing, which stays as it is under it. The way back is the page's way, button 0: the
		// rows are 1 and up, as on a page. a_keepRow: this one again (a key typed), with the same row chosen.
		public function SetDialog(a_serial:Number, a_title:String, a_text:String, a_rows:Array, a_keepRow:Boolean, a_hasInput:Boolean, a_input:String):void
		{
			var keptRow:int = 0;
			if (dialog != null) {
				keptRow = dialog.rows.selectedIndex;
				content.removeChild(dialog);
			}
			held = 0;
			serial = a_serial;
			var labels:Array = [];
			for (var i:int = 0; i < a_rows.length; ++i) {
				labels.push(Style.Text(String(a_rows[i]), TEXT_SIZE * scale, color, scale));
			}
			dialog = new Dialog(Pressed, function():void { Code("PlaySound", "UIMenuFocus"); });
			dialog.Build(a_title, a_text, labels, a_hasInput, a_input, shownWidth, panelHeight, { pitch: ROW_PITCH * scale, barHeight: BAR_HEIGHT * scale }, color, scale);
			if (a_keepRow) {
				dialog.rows.Restore(keptRow, 0);
			}
			content.addChild(dialog);
		}

		// From the list's scrollbar and the wheel: how far along it is, 0 to 1.
		private function PlayersTo(a_at:Number):void
		{
			if (players == null) {
				return;
			}
			playersAt = Math.max(0, Math.min(1, a_at));
			var over:Number = Math.max(0, playersHeight - playersShown);
			players.y = Math.round(playersTop - over * playersAt);
			if (playersBar != null) {
				playersBar.Update(playersShown / playersHeight, playersAt);
			}
		}

		// The list at the right of the main page's rows: each player's name, their marker as on the compass, and how they are; or a
		// line of text.
		private function Players(a_rows:Array, a_width:Number, a_choosable:Boolean):Sprite
		{
			var out:Sprite = new Sprite();
			var y:Number = 0;
			var markerRoom:Number = MARKER_ROOM * scale;
			var inset:Number = PLAYERS_INSET * scale;
			for (var i:int = 0; i < a_rows.length; ++i) {
				var parts:Array = String(a_rows[i]).split("\t");
				var isPlayer:Boolean = parts.length >= 4;
				var row:Sprite = new Sprite();
				// A player's name leaves room for their marker after it.
				var name:TextField = Style.Wrapped(isPlayer ? String(parts[2]) : String(parts[0]), a_width - (isPlayer ? markerRoom : 0), BODY_SIZE * scale, color, scale);
				var stats:String = isPlayer ? String(parts[3]).toUpperCase() : "";  // in capitals, as the game's own HP and LEVEL
				var where:String = isPlayer && parts.length > 4 ? String(parts[4]) : "";
				var said:String = stats + (stats != "" && where != "" ? " - " : "") + where;
				var details:TextField = said != "" ? Style.Wrapped(said, a_width, DETAILS_SIZE * scale, color, scale) : null;
				var needs:Number = name.height + (details != null ? details.height - DETAILS_RISE * scale : 0);
				if (isPlayer) {
					var bar:Shape = new Shape();
					bar.name = "bar";
					bar.graphics.beginFill(color);
					bar.graphics.drawRect(-inset, 0, a_width + inset * 2, needs);
					bar.graphics.endFill();
					bar.visible = false;
					row.addChild(bar);
				}
				name.name = "name";
				row.addChild(name);
				if (isPlayer) {
					var marker:Shape = Marker(uint("0x" + String(parts[0])), int(parts[1]));
					marker.x = name.textWidth + markerRoom * 0.5;
					marker.y = name.height / 2;
					row.addChild(marker);
				}
				if (details != null) {
					details.name = "details";
					details.y = name.height - DETAILS_RISE * scale;
					row.addChild(details);
				}
				row.y = y;
				out.addChild(row);
				if (isPlayer && a_choosable) {
					AddPlayerRow(row, playerRows.length);
					playerRows.push(row);
				}
				y += needs + PLAYER_GAP * scale;
			}
			return out;
		}

		// The mouse on a player's row: only one that moved chooses it, as a row built under a still one rolls over too.
		private function AddPlayerRow(a_row:Sprite, a_index:int):void
		{
			a_row.buttonMode = true;
			a_row.addEventListener(MouseEvent.ROLL_OVER, function(a_e:MouseEvent):void {
				if (mouseX != builtMouseX || mouseY != builtMouseY) {
					if (chosenPlayer != a_index) {
						Code("PlaySound", "UIMenuFocus");
						ChoosePlayer(a_index);
					}
				}
			});
			a_row.addEventListener(MouseEvent.CLICK, function(a_e:MouseEvent):void { Pressed(list.length + a_index); });
		}

		// A player's row as chosen, as the rows are: the bar in the color, the text black. -1: the choice is back in the rows.
		private function ChoosePlayer(a_index:int):void
		{
			if (chosenPlayer >= 0 && chosenPlayer < playerRows.length) {
				PaintPlayer(playerRows[chosenPlayer], false);
			}
			var was:int = chosenPlayer;
			chosenPlayer = a_index;
			if (a_index < 0) {
				if (was >= 0) {
					list.SetActive(true);
				}
				return;
			}
			list.SetActive(false);
			var row:Sprite = playerRows[a_index];
			PaintPlayer(row, true);
			// Scrolled so it shows.
			var over:Number = playersHeight - playersShown;
			if (playersBar != null && over > 0) {
				var top:Number = playersTop - players.y;
				if (row.y < top) {
					PlayersTo(row.y / over);
				} else if (row.y + row.height > top + playersShown) {
					PlayersTo((row.y + row.height - playersShown) / over);
				}
			}
		}

		private function PaintPlayer(a_row:Sprite, a_on:Boolean):void
		{
			a_row.getChildByName("bar").visible = a_on;
			(a_row.getChildByName("name") as TextField).textColor = a_on ? Style.BLACK : color;
			var details:TextField = a_row.getChildByName("details") as TextField;
			if (details != null) {
				details.textColor = a_on ? Style.BLACK : color;
			}
		}

		// The compass's marker (Compass.cpp's DrawShape), centered on (0, 0), with no outline.
		private function Marker(a_color:uint, a_shape:int):Shape
		{
			var size:Number = MARKER_SIZE * scale;
			var shape:Shape = new Shape();
			shape.graphics.beginFill(a_color);
			switch (a_shape) {
				case 0:
					shape.graphics.drawCircle(0, 0, 6 * size);
					break;
				case 1:
					shape.graphics.drawRect(-5.5 * size, -5.5 * size, 11 * size, 11 * size);
					break;
				case 2:
					shape.graphics.moveTo(0, -7.5 * size);
					shape.graphics.lineTo(6.5 * size, 0);
					shape.graphics.lineTo(0, 7.5 * size);
					shape.graphics.lineTo(-6.5 * size, 0);
					shape.graphics.lineTo(0, -7.5 * size);
					break;
				default:
					shape.graphics.moveTo(0, 6.5 * size);
					shape.graphics.lineTo(7 * size, -6 * size);
					shape.graphics.lineTo(-7 * size, -6 * size);
					shape.graphics.lineTo(0, 6.5 * size);
					break;
			}
			shape.graphics.endFill();
			return shape;
		}

		// Where the rows start under what ends at a_y, leaving the chevrons their room.
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
				ShowCommandBar();
			}
		}

		// The game's user events, keyboard and gamepad alike. True: taken here.
		public function ProcessUserEvent(a_event:String, a_pressed:Boolean):Boolean
		{
			// The player's movement keys come as Forward and Back (Back is a direction here, not the way back: that's Cancel).
			if (a_pressed && dialog == null && playerRows.length > 0 && (a_event == "Left" || a_event == "StrafeLeft" || a_event == "Right" || a_event == "StrafeRight")) {
				var toList:Boolean = a_event == "Right" || a_event == "StrafeRight";
				if (toList != (chosenPlayer >= 0)) {
					Code("PlaySound", "UIMenuFocus");
					ChoosePlayer(toList ? 0 : -1);
				}
				return true;
			}
			if (a_event == "Up" || a_event == "Down" || a_event == "Forward" || a_event == "Back") {
				held = a_pressed ? (a_event == "Up" || a_event == "Forward" ? -1 : 1) : 0;
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
				case "Activate":  // the player's activate key, as well as the menus' own Enter and the gamepad's A
					if (dialog != null) {
						dialog.rows.PressSelected();
					} else if (chosenPlayer >= 0) {
						Pressed(list.length + chosenPlayer);
					} else {
						list.PressSelected();
					}
					return true;
				case "Cancel":
					Back();
					return true;
				case "LShoulder":
				case "RShoulder":
					// The gamepad's bumpers page a long text on any page: Up and Down move between the rows, and scroll the text only
					// on a page with none to move between.
					if (dialog == null && bodyField != null && bodyField.maxScrollV > 1) {
						ScrollBody((a_event == "LShoulder" ? -1 : 1) * Math.max(1, bodyField.bottomScrollV - bodyField.scrollV));
						return true;
					}
					return false;
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
			if (dialog != null) {
				dialog.rows.Move(a_by);
			} else if (chosenPlayer >= 0) {
				ChoosePlayer(Math.max(0, Math.min(playerRows.length - 1, chosenPlayer + a_by)));
			} else if (list.length > 1) {
				list.Move(a_by);
			} else {
				ScrollBody(a_by);  // with no rows to move between, or the one
			}
		}

		// Over the text (when it scrolls) the text, else the rows.
		private function Wheel(a_e:MouseEvent):void
		{
			if (dialog != null) {
				return;
			}
			var by:int = a_e.delta > 0 ? -1 : 1;
			if (playersBar != null && content.mouseX >= playersX) {
				PlayersTo(playersAt + by * WHEEL_ROW * scale / Math.max(1, playersHeight - playersShown));
				return;
			}
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
			if (centered || BGSCodeObj == null || panel.hitTestPoint(a_e.stageX, a_e.stageY, false) || commandBar.hitTestPoint(a_e.stageX, a_e.stageY, true)) {
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
