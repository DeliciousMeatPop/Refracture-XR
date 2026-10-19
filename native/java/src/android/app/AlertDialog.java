package android.app;

import android.content.Context;
import android.content.DialogInterface;
import android.view.View;

/** Dialogs aren't shown; titles and messages go to the log so fatal errors are visible. */
public class AlertDialog extends Dialog {
    protected AlertDialog(Context context) { super(context); }

    public static class Builder {
        private final Context context;
        private boolean cancelable = true;

        public Builder(Context context) { this.context = context; }

        public Builder(Context context, int themeResId) { this(context); }

        public Context getContext() { return context; }

        public Builder setTitle(CharSequence title) {
            refract.Runtime.log(6, "AlertDialog", "title: " + title);
            return this;
        }

        public Builder setTitle(int titleId) { return this; }

        public Builder setMessage(CharSequence message) {
            refract.Runtime.log(6, "AlertDialog", "message: " + message);
            return this;
        }

        public Builder setMessage(int messageId) { return this; }

        public Builder setIcon(int iconId) { return this; }

        public Builder setView(View view) { return this; }

        public Builder setView(int layoutResId) { return this; }

        public Builder setCancelable(boolean cancelable) {
            this.cancelable = cancelable;
            return this;
        }

        public Builder setPositiveButton(CharSequence text, DialogInterface.OnClickListener listener) { return this; }

        public Builder setPositiveButton(int textId, DialogInterface.OnClickListener listener) { return this; }

        public Builder setNegativeButton(CharSequence text, DialogInterface.OnClickListener listener) { return this; }

        public Builder setNegativeButton(int textId, DialogInterface.OnClickListener listener) { return this; }

        public Builder setNeutralButton(CharSequence text, DialogInterface.OnClickListener listener) { return this; }

        public Builder setNeutralButton(int textId, DialogInterface.OnClickListener listener) { return this; }

        public Builder setOnCancelListener(DialogInterface.OnCancelListener listener) { return this; }

        public Builder setOnDismissListener(DialogInterface.OnDismissListener listener) { return this; }

        public Builder setOnKeyListener(DialogInterface.OnKeyListener listener) { return this; }

        public Builder setItems(CharSequence[] items, DialogInterface.OnClickListener listener) { return this; }

        public AlertDialog create() {
            AlertDialog d = new AlertDialog(context);
            d.setCancelable(cancelable);
            return d;
        }

        public AlertDialog show() {
            AlertDialog d = create();
            d.show();
            return d;
        }
    }
}
