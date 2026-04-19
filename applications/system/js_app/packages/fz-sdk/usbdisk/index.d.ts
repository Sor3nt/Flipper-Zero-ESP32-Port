/**
 * Module for USB mass storage emulation
 * @version Available with JS feature `usbdisk`
 * @module
 */

/**
 * @brief Start emulating mass storage device
 * @param path The disk image to emulate
 */
export declare function start(path: string): void;

/**
 * @brief Stop emulating mass storage device
 */
export declare function stop(): void;

/**
 * @brief Check if the mass storage device was exected
 * 
 * Useful as a loop condition with a delay, so UsbDisk keeps running until ejected
 * 
 */
export declare function wasEjected(): boolean;
