# SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.esp32p4
#@pytest.mark.usb_device             Disable in CI: unavailable teardown for P4
def test_usb_device_dconn_detection(dut: IdfDut) -> None:
    dut.run_all_single_board_cases(group='dconn')
