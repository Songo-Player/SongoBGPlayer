package org.songo.harness;

import android.app.Activity;
import android.view.View;

import org.godotengine.godot.Godot;
import org.godotengine.godot.plugin.GodotPlugin;
import org.godotengine.godot.plugin.UsedByGodot;

/**
 * Godot v2 plugin entry point, registered by the meta-data in this AAR's
 * manifest. Starts following the engine as Songo starts, so background
 * playback needs nothing from GDScript.
 */
public class SongoHarnessPlugin extends GodotPlugin {
	private boolean active;

	public SongoHarnessPlugin(Godot godot) {
		super(godot);
	}

	@Override
	public String getPluginName() {
		return "SongoHarness";
	}

	@Override
	public View onMainCreate(Activity activity) {
		active = SongoPlayback.get(activity).start();
		return null;
	}

	@Override
	public void onMainDestroy() {
		Activity activity = getActivity();
		if (activity != null) {
			SongoPlayback.get(activity).shutdown();
		}
		active = false;
	}

	/** Whether background playback is set up, for checking from GDScript. */
	@UsedByGodot
	public boolean isBackgroundPlaybackActive() {
		return active;
	}
}
