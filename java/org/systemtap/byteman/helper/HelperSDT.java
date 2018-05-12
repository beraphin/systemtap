package org.systemtap.byteman.helper;
import java.io.StringWriter;
import java.io.PrintWriter;
import java.lang.Throwable;

public class HelperSDT<T2, T3, T4, T5, T6, T7, T8, T9, T10, T11>
{
    public void STAP_BACKTRACE(String rulename){
	Throwable e = new Throwable();
	StringWriter sw = new StringWriter();
	e.printStackTrace(new PrintWriter(sw));
	String exceptionAsString = sw.toString();
	String[] stackline = exceptionAsString.split("\n");
	int __counter = 0;
	for(String result : stackline){
	    METHOD_STAP_BT(rulename, result, __counter);
	    __counter++;
	}
    }
    public native void METHOD_STAP_PROBE0(String rulename);
    public native void METHOD_STAP_PROBE1(String rulename, T2 arg1);
    public native void METHOD_STAP_PROBE2(String rulename, T2 arg1, T3 arg2);
    public native void METHOD_STAP_PROBE3(String rulename, T2 arg1, T3 arg2, T4 arg3);
    public native void METHOD_STAP_PROBE4(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4);
    public native void METHOD_STAP_PROBE5(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5);
    public native void METHOD_STAP_PROBE6(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5, T7 arg6);
    public native void METHOD_STAP_PROBE7(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5, T7 arg6, T8 arg7);
    public native void METHOD_STAP_PROBE8(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5, T7 arg6, T8 arg7, T9 arg8);
    public native void METHOD_STAP_PROBE9(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5, T7 arg6, T8 arg7, T9 arg8, T10 arg9);
    public native void METHOD_STAP_PROBE10(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5, T7 arg6, T8 arg7, T9 arg8, T10 arg9, T11 arg10);
    /* systemtap 3.1 java abi */
    public native void METHOD_STAP31_PROBE0(String rulename);
    public native void METHOD_STAP31_PROBE1(String rulename, T2 arg1);
    public native void METHOD_STAP31_PROBE2(String rulename, T2 arg1, T3 arg2);
    public native void METHOD_STAP31_PROBE3(String rulename, T2 arg1, T3 arg2, T4 arg3);
    public native void METHOD_STAP31_PROBE4(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4);
    public native void METHOD_STAP31_PROBE5(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5);
    public native void METHOD_STAP31_PROBE6(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5, T7 arg6);
    public native void METHOD_STAP31_PROBE7(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5, T7 arg6, T8 arg7);
    public native void METHOD_STAP31_PROBE8(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5, T7 arg6, T8 arg7, T9 arg8);
    public native void METHOD_STAP31_PROBE9(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5, T7 arg6, T8 arg7, T9 arg8, T10 arg9);
    public native void METHOD_STAP31_PROBE10(String rulename, T2 arg1, T3 arg2, T4 arg3, T5 arg4, T6 arg5, T7 arg6, T8 arg7, T9 arg8, T10 arg9, T11 arg10);
    public native void METHOD_STAP_BT(String rulename, String exceptionAsString, int __counter);
    public native void METHOD_BT_DELETE(String rulename);
    static{
        System.loadLibrary("HelperSDT_" + System.getProperty("os.arch"));
    }
}
